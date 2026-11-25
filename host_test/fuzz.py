import random, subprocess, sys
sys.argv=["x"]
src=open("test.py").read().split("cases = [")[0]
src=src.replace('m = run(MN,"","mnemonic")','m=None#').replace("assert m ==","pass#").replace("for pw in","for pw in []:#").replace('print("bip39 ok")','').replace('print("info/xpub/descriptor ok")','')
ns={}; exec(src, ns)
MN=ns["MN"]; rnd=random.Random(int(sys.argv[1]) if len(sys.argv)>1 else 7); crashes=0
bases=[ns["build"](0,"",["wpkh","tr"],"tr",True,None)[0].serialize(), ns["build"](1,"",["tr"],"wpkh",False,1)[0].serialize()]
for i in range(3000):
    b = bytearray(rnd.choice(bases)); op = rnd.randrange(3)
    if op==0: b = b[:rnd.randrange(len(b))]
    elif op==1:
        for _ in range(rnd.randrange(1,6)): b[rnd.randrange(len(b))] = rnd.randrange(256)
    else:
        pos=rnd.randrange(len(b)); b[pos:pos]=bytes([rnd.choice([0xfd,0xfe,0xff,0x00,0x22])])
    open("fz.psbt","wb").write(b)
    r = subprocess.run(["./t_asan",MN,"","sign","fz.psbt","fzo"],capture_output=True,text=True)
    if r.returncode not in (0,1) or "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        crashes+=1; print(r.stderr[-800:]); break
print("fuzz: 3000 iteracoes, crashes:", crashes)
