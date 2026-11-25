import subprocess, json, random
from embit import bip39, bip32, script, ec
from embit.descriptor import Descriptor
from embit.psbt import PSBT
from embit.transaction import Transaction, TransactionInput, TransactionOutput
from embit.networks import NETWORKS

MN = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
def run(*a):
    return subprocess.run(["./t_sign", *a], capture_output=True, text=True)

# 1. mnemonic from entropy 00..1f
m = run(MN,"","mnemonic").stdout.strip()
assert m == bip39.mnemonic_from_bytes(bytes(range(32))), m
print("bip39 ok")

for pw in ["", "pokemon:74:Geodude"]:
  for testnet in [0,1]:
    seed = bip39.mnemonic_to_seed(MN, pw)
    root = bip32.HDKey.from_seed(seed)
    net = NETWORKS["test" if testnet else "main"]
    info = json.loads(run(MN,pw,"info",str(testnet)).stdout)
    assert info["fingerprint"] == root.my_fingerprint.hex()
    coin = 1 if testnet else 0
    for acc in info["accounts"]:
        purpose = 84 if acc["type"]=="wpkh" else 86
        x = root.derive(f"m/{purpose}h/{coin}h/0h").to_public()
        assert acc["xpub"] == x.to_base58(version=net["xpub"]), acc["type"]
        for k in ("receive","change"):
            d = Descriptor.from_string(acc[k])  # checks checksum
            from embit.descriptor.checksum import add_checksum
            assert add_checksum(acc[k].split("#")[0]) == acc[k]
print("info/xpub/descriptor ok")

def build(testnet, pw, kinds, ext_kind="wpkh", foreign=False, sighash=None, rnd=random.Random(1)):
    seed = bip39.mnemonic_to_seed(MN, pw); root = bip32.HDKey.from_seed(seed)
    coin = 1 if testnet else 0
    fp = root.my_fingerprint
    ins, utxos, keys = [], [], []
    for i,k in enumerate(kinds):
        purpose = 84 if k=="wpkh" else 86
        path = f"m/{purpose}h/{coin}h/0h/0/{i}"
        child = root.derive(path)
        pub = child.key.get_public_key()
        sc = script.p2wpkh(pub) if k=="wpkh" else script.p2tr(pub)
        txid = bytes(rnd.getrandbits(8) for _ in range(32))
        ins.append(TransactionInput(txid, i)); utxos.append(TransactionOutput(100000*(i+1), sc)); keys.append((k,path,pub,child))
    if foreign:
        other = ec.PrivateKey(bytes([7]*32))
        ins.append(TransactionInput(bytes(32*[9]),0)); utxos.append(TransactionOutput(50000, script.p2wpkh(other.get_public_key())))
    total = sum(u.value for u in utxos)
    dest = script.p2wpkh(ec.PrivateKey(bytes([3]*32)).get_public_key())
    chg_path = f"m/{84 if ext_kind=='wpkh' else 86}h/{coin}h/0h/1/5"
    chg = root.derive(chg_path).key.get_public_key()
    chg_sc = script.p2wpkh(chg) if ext_kind=="wpkh" else script.p2tr(chg)
    outs = [TransactionOutput(70000, dest), TransactionOutput(total-70000-1500, chg_sc)]
    tx = Transaction(vin=ins, vout=outs, locktime=0)
    p = PSBT(tx)
    for i,u in enumerate(utxos):
        p.inputs[i].witness_utxo = u
        if sighash is not None: p.inputs[i].sighash_type = sighash
    for i,(k,path,pub,child) in enumerate(keys):
        der = bip32.parse_path(path)
        from embit.psbt import DerivationPath
        if k=="wpkh":
            p.inputs[i].bip32_derivations[pub] = DerivationPath(fp, der)
        else:
            p.inputs[i].taproot_bip32_derivations[pub] = ([], DerivationPath(fp, der))
            p.inputs[i].taproot_internal_key = pub
    from embit.psbt import DerivationPath
    if ext_kind=="wpkh": p.outputs[1].bip32_derivations[chg] = DerivationPath(fp, bip32.parse_path(chg_path))
    else:
        p.outputs[1].taproot_bip32_derivations[chg] = ([], DerivationPath(fp, bip32.parse_path(chg_path)))
        p.outputs[1].taproot_internal_key = chg
    return p, keys, utxos

def verify(p_signed, keys, utxos, sighash):
    tx = p_signed.tx
    n=0
    for i,(k,path,pub,child) in enumerate(keys):
        inp = p_signed.inputs[i]
        if k=="wpkh":
            sig = inp.partial_sigs[pub]
            assert sig[-1]==1
            h = tx.sighash_segwit(i, script.p2pkh_from_p2wpkh(utxos[i].script_pubkey), utxos[i].value)
            assert pub.verify(ec.Signature.parse(sig[:-1]), h), "ecdsa"
        else:
            sig = inp.unknown[b"\x13"]
            ht = 0 if sighash is None else sighash
            assert len(sig)==(64 if ht==0 else 65)
            h = tx.sighash_taproot(i, script_pubkeys=[u.script_pubkey for u in utxos], values=[u.value for u in utxos], sighash=ht)
            outpub = pub.taproot_tweak(b"")
            assert outpub.schnorr_verify(ec.SchnorrSig.parse(sig[:64]), h), "schnorr"
        n+=1
    return n

cases = [(0,"",["wpkh"],"wpkh",False,None),(1,"",["tr"],"tr",False,None),(0,"pokemon:74:Geodude",["wpkh","tr","wpkh"],"tr",False,None),
         (1,"",["tr","wpkh"],"wpkh",True,None),(0,"",["tr","wpkh"],"wpkh",False,1)]
for c in cases:
    p,keys,utxos = build(*c)
    open("in.psbt","wb").write(p.serialize())
    r = run(MN,c[1],"sign","in.psbt","out.psbt")
    print(r.stderr); assert r.stdout.strip()=="OK", r.stdout
    ps = PSBT.parse(open("out.psbt","rb").read())
    print("case",c,"verified sigs:",verify(ps,keys,utxos,c[5]))
    # second pass must find nothing to sign
    open("in2.psbt","wb").write(ps.serialize())
    assert "ERR -3" in run(MN,c[1],"sign","in2.psbt","x").stdout

# negative: wrong passphrase -> nothing to sign
p,keys,utxos = build(0,"",["wpkh"]); open("in.psbt","wb").write(p.serialize())
print("wrong pw:", run(MN,"zzz","sign","in.psbt","o").stdout.strip())
# negative: tampered derivation pubkey
p,keys,utxos = build(0,"",["wpkh"]); 
k=list(p.inputs[0].bip32_derivations.keys())[0]; d=p.inputs[0].bip32_derivations.pop(k)
p.inputs[0].bip32_derivations[ec.PrivateKey(bytes([5]*32)).get_public_key()]=d
open("in.psbt","wb").write(p.serialize()); print("tampered:", run(MN,"","sign","in.psbt","o").stdout.strip())
# negative: taproot with foreign input lacking utxo
p,keys,utxos = build(0,"",["tr"],foreign=True); p.inputs[1].witness_utxo=None
open("in.psbt","wb").write(p.serialize()); print("tr missing utxo:", run(MN,"","sign","in.psbt","o").stdout.strip())
# negative: unsupported sighash
p,keys,utxos = build(0,"",["wpkh"],sighash=3); open("in.psbt","wb").write(p.serialize()); print("sighash 3:", run(MN,"","sign","in.psbt","o").stdout.strip())
# change-address attack: change path ours but spk swapped -> must be shown as destination
p,keys,utxos = build(0,"",["wpkh"]); p.tx.vout[1].script_pubkey = script.p2wpkh(ec.PrivateKey(bytes([4]*32)).get_public_key())
open("in.psbt","wb").write(p.serialize()); r=run(MN,"","sign","in.psbt","o"); print("fake change ->\n"+r.stderr)
p,keys,utxos = build(0,"",["wpkh"]); p.outputs[1].script_pubkey = script.p2wpkh(ec.PrivateKey(bytes([4]*32)).get_public_key())
open("in.psbt","wb").write(p.serialize()); r=run(MN,"","sign","in.psbt","o"); print("fake change v2 ->\n"+r.stderr)
assert "Troco (verificado): 0.00000000" in r.stderr
# non_witness_utxo: build prev tx for real
seed = bip39.mnemonic_to_seed(MN,""); root = bip32.HDKey.from_seed(seed)
pub = root.derive("m/84h/0h/0h/0/0").key.get_public_key()
prev = Transaction(vin=[TransactionInput(bytes(32),0)], vout=[TransactionOutput(1,script.p2wpkh(pub)),TransactionOutput(200000,script.p2wpkh(pub))])
for wu in [True, False]:
  p,keys,utxos = build(0,"",["wpkh"])
  p.tx.vin[0].txid = prev.txid(); p.tx.vin[0].vout = 1
  p.inputs[0].txid = prev.txid(); p.inputs[0].vout = 1
  p.inputs[0].non_witness_utxo = prev
  if not wu: p.inputs[0].witness_utxo = None
  else: p.inputs[0].witness_utxo = prev.vout[1]
  p.outputs[1].value = 200000-70000-1500
  open("in.psbt","wb").write(p.serialize()); r=run(MN,"","sign","in.psbt","out.psbt")
  ps = PSBT.parse(open("out.psbt","rb").read())
  print("nwu (witness_utxo too=%s):"%wu, r.stdout.strip(), verify(ps,keys,[prev.vout[1]],None))
# lying witness_utxo amount vs non_witness_utxo
p.inputs[0].witness_utxo = TransactionOutput(150000, script.p2wpkh(pub))
open("in.psbt","wb").write(p.serialize()); print("lying amount:", run(MN,"","sign","in.psbt","o").stdout.strip())
print("ALL TESTS PASSED")
