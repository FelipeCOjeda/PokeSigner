# Passo a passo: "Cofre Signy" na Confidential Wallet

Objetivo: adicionar à Confidential Wallet (`FelipeCOjeda/confidential-wallet`, Kotlin + Breez SDK Spark) um **cofre on-chain protegido pelo PokeSigner/Signy**.

- A carteira quente (Spark, Lightning, DePix) continua no Breez SDK, que guarda a própria seed e assina tudo internamente. **O Signy não assina operações Spark/Lightning**: o SDK precisa da seed em memória.
- O cofre é uma conta **BIP84 (bc1q) ou BIP86 (bc1p)** cuja seed só existe na placa. O app conhece apenas a xpub (watch-only), monta PSBTs e o Signy assina depois da aprovação na tela.
- Ponte entre os dois: "Guardar no cofre" (Spark → endereço do cofre) e "Trazer para a carteira" (cofre → endereço on-chain da Spark).

Referências: protocolo BLE e PSBT em `INTEGRACAO-POKEWALLET.md`. O código pronto para reaproveitar está na PokeWallet (mesmo autor, Kotlin, sem bibliotecas de carteira).

---

## Passo 0: dependências e permissões

`build.gradle.kts`: o BouncyCastle (`bcprov-jdk18on`) e as coroutines já estão lá. Nada novo é obrigatório.

`AndroidManifest.xml` (Bluetooth LE):
```xml
<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" tools:targetApi="s" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-feature android:name="android.hardware.bluetooth_le" android:required="false" />
```
Peça as permissões em tempo de execução antes do primeiro scan.

## Passo 1: trazer o núcleo Bitcoin da PokeWallet

Copie para `com.carteira.wallet.btc` (ajustando o `package`) os arquivos da PokeWallet:

| Arquivo (PokeWallet) | Para quê |
|---|---|
| `crypto/Hashes.kt`, `crypto/HexExtensions.kt` | sha256, hash160, hex |
| `crypto/Secp256k1.kt` | chaves, tweak taproot (BouncyCastle) |
| `crypto/KeyDerivation.kt` (só `hardened()`) | caminhos BIP32 |
| `network/XpubAddressDeriver.kt` | derivar endereços da xpub (sem seed) |
| `crypto/Psbt.kt`, `crypto/PsbtTaproot.kt`, `crypto/TaprootPsbtInput.kt`, `crypto/Bip32Derivation.kt` | PSBT v0 (BIP174/371) |
| `crypto/PsbtAssembler.kt` | montar o PSBT com derivações e troco |
| `crypto/SpendResolver.kt` (ou uma versão simples) | escolha de UTXOs e taxa |

Ajuste obrigatório: em `PsbtTaproot.parseBase64()`, ler a assinatura que volta do Signy:
```kotlin
0x13 -> input.tapKeySig = value
```
E crie `SignyPsbt.finalizeSigned(bytes): String` (hex da tx). O código está em `INTEGRACAO-POKEWALLET.md`, seção 3.2.

## Passo 2: cliente Bluetooth

Copie o `PokeSignerClient.kt` da integração da PokeWallet (ou implemente pelo esqueleto da seção 3.3 do outro guia) como `SignyClient.kt`. Ele precisa de:
- `findDevice()`: scan filtrado pelo serviço `50530001-b5a3-f393-e0a9-50ca5e1d0001`;
- `connect()`: MTU 517, bond (código de 6 dígitos na tela da placa), notificações na `…0003`;
- `getInfo(testnet)` → JSON com fingerprint e xpubs;
- `signPsbt(bytes, onStatus)`: espera **`0x82`** (PSBT assinado) ou **`0xFF`** (erro). Os quadros **`0x90` são status e não encerram a espera**. Timeout de 3 min;
- `confirm(code)` (`0x04`), `next()` (`0x06`), `prev()` (`0x07`), `cancel()` (`0x05`) para pilotar a revisão pelo celular.

## Passo 3: guardar o cofre (`VaultStore.kt`)

Só dados públicos, então `SharedPreferences` comum serve:
```kotlin
data class Vault(
    val fingerprint: String,      // "94f97265"
    val type: String,             // "wpkh" | "tr"
    val xpub: String,
    val origin: String,           // "[94f97265/84h/0h/0h]xpub..."
    val network: String,          // "main" | "test"
    var nextReceive: Int = 0,
    var nextChange: Int = 0,
    val deviceName: String        // "PokeSigner-6526"
)
```
**Nunca** derive o cofre da seed da Spark: o cofre só faz sentido se a seed dele estiver só na placa.

## Passo 4: tela "Conectar Signy"

1. Botão **"🤖 Conectar Signy"** na tela principal (só aparece se não houver cofre).
2. Checa permissões e Bluetooth ligado → `findDevice()` → `connect()`. O Android abre o diálogo de código, e o usuário digita o que aparece na placa.
3. `getInfo(testnet = false)` → o usuário escolhe **Native SegWit (bc1q)** ou **Taproot (bc1p)** → monta `origin = "[fp/" + path.removePrefix("m/") + "]" + xpub` → salva o `Vault`.
4. Mostra o primeiro endereço de recebimento e pede para o usuário conferir na placa, com um comando futuro de "mostrar endereço". Por enquanto, conferir com outra carteira importando o mesmo descriptor.

## Passo 5: saldo e UTXOs do cofre

Use a API Esplora (mempool.space ou blockstream.info; a PokeWallet já tem `BlockstreamClient`):
- Varre os endereços `m/…/0/i` e `m/…/1/i` com **gap limit 20**: `GET /address/{addr}` (histórico) e `GET /address/{addr}/utxo`.
- Guarda os UTXOs com `(chain, index)`, que o `PsbtAssembler` precisa para as derivações.
- Atualiza ao abrir a tela e com "puxar para atualizar".

## Passo 6: receber no cofre

- Mostra `deriveAddress(chain = 0, index = nextReceive)` com QR.
- Quando aparecer histórico nesse endereço, faz `nextReceive++`.

## Passo 7: "Guardar no cofre" (Spark → cofre)

Usa o que a carteira já tem: `WalletManager.prepareSweepToOnchain(fixedAddress)` (tudo) ou `prepareSend(endereçoDoCofre, valor)` (parcial), com o endereço vindo do Passo 6. Confirmação normal da Spark. A placa não participa.

## Passo 8: enviar do cofre (precisa do Signy)

1. O usuário escolhe destino, valor e taxa. Para sugerir a taxa: `GET https://mempool.space/api/v1/fees/recommended`.
2. Seleção de UTXOs + troco em `m/…/1/nextChange` → `PsbtAssembler.build(..., masterFingerprint = fingerprint real)`.
3. Diálogo **"Aprovar no Signy"**:
   - `signPsbt(bytes) { status -> mostra }`;
   - botões ◀ ▶ ✖ e campo **"código da tela (6 dígitos)"** → `confirm(code)`. O app nunca sabe o código: o usuário lê na placa;
   - quem prefere pode aprovar segurando BOOT na placa.
4. Recebeu `0x82`: `SignyPsbt.finalizeSigned()` → confere que o txid é igual ao `expectedTxid` → `POST /tx` (Esplora) → mostra o txid. Faz `nextChange++` se usou troco.
5. `0xFF`: mostra a mensagem da placa ("rejeitado no dispositivo", "tempo esgotado", "PSBT e da carteira X…").

## Passo 9: "Trazer para a carteira" (cofre → Spark)

Igual ao Passo 8, com o destino preenchido por `WalletManager.receiveBitcoinAddress()` (endereço de depósito on-chain da Spark). Depois da confirmação, o saldo aparece na Spark e pode ir para Lightning ou DePix.

## Passo 10: interface

- Tela principal: cartão **Carteira** (Spark: saldo, receber, enviar, Lightning, DePix) e cartão **Cofre Signy** (saldo on-chain, receber, guardar, trazer, enviar).
- Mostre o estado da placa: procurando → conectando → "Revise na placa" → assinado → transmitido. Use as poses do Signy (procurando = pensando, pareado = coração, assinado = check, erro = X).

## Passo 11: testes

1. Mainnet com valores pequenos (a Spark só roda em mainnet): receber ~10.000 sats no cofre → atualizar saldo → enviar 5.000 para outro endereço seu → aprovar na placa → conferir no mempool.space.
2. "Guardar" e "Trazer" com 20.000 sats.
3. Casos de erro: placa desligada; rejeitar na placa; código errado 3×; cofre de outra seed (a placa mostra os dois fingerprints).

## Avisos

- O cofre fica tão seguro quanto o firmware e a placa. A Confidential Wallet ainda guarda a seed da Spark em `SharedPreferences` (MVP), e isso não muda com o cofre.
- Futuro: um comando no firmware que **mostra o endereço de recebimento na tela da placa**, para conferência independente do celular.
- Futuro: usar o Signy para **destravar a seed da Spark** (a placa devolve um segredo derivado que cifra a seed no celular). Exige um comando novo no firmware.
