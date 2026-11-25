# Integração PokeWallet ⇄ PokeSigner (BLE)

Guia para implementar, na PokeWallet (Android/Kotlin), o uso do **PokeSigner**:
assinador de PSBT em ESP32-C6 (placa Waveshare ESP32-C6-LCD-1.47) que conversa por Bluetooth LE.

- Firmware/protocolo: https://github.com/FelipeCOjeda/PokeSigner (`main/ble.c`, `main/ble.h`, `tools/client.py`)
- Instalador web: https://felipecojeda.github.io/PokeSigner/
- Estado: firmware v0.4.0 (v0.3.2 testado em placa real. O pareamento com o celular funcionou (nRF Connect)).

---

## 1. Ideia geral (reaproveita o fluxo air-gapped que já existe)

A PokeWallet já tem tudo para isso no modo **watch-only + air-gapped**:

| Etapa | Hoje (QR) | Com PokeSigner (BLE) |
|---|---|---|
| Importar carteira | colar `[fp/84h/0h/0h]xpub` em `WalletWatchOnlyImport` | **ler do PokeSigner** (`GET_INFO`) e preencher o mesmo campo |
| Montar PSBT | `PsbtAssembler.build()` (já põe fingerprint, derivações, witness_utxo, troco) | **igual, sem mudança** |
| Assinar | QR → outro aparelho → QR da tx | **envia o PSBT por BLE**; o usuário aprova na tela da placa; volta o **PSBT assinado** |
| Transmitir | `submitSignedAirGappedTx(rawHex, expectedTxid, …)` | finaliza o PSBT e chama **a mesma função** (ela já confere o txid) |

A diferença principal: o PokeSigner devolve um **PSBT com assinatura parcial**, e não a tx bruta. O app precisa finalizar.

---

## 2. Protocolo BLE

### GATT
| Item | UUID |
|---|---|
| Serviço | `50530001-b5a3-f393-e0a9-50ca5e1d0001` |
| RX (app → placa): write / write-without-response, **exige link cifrado e autenticado** | `50530002-b5a3-f393-e0a9-50ca5e1d0001` |
| TX (placa → app): notify | `50530003-b5a3-f393-e0a9-50ca5e1d0001` |

- Nome anunciado: `PokeSigner-XXXX` (scan response) e `PS-XXXX` (no próprio anúncio). O anúncio traz o UUID do serviço, então dá para filtrar o scan por ele.
- MTU: peça **517** (`requestMtu`). Fatie as escritas em `mtu - 3` (no máximo 512).
- **Segurança:** LE Secure Connections com MITM. A placa é *DisplayOnly*: mostra um **código de 6 dígitos na tela** e o Android abre o diálogo do sistema para digitá-lo. O bond fica salvo nos dois lados. A placa também pede a segurança logo ao conectar.

### Quadro (frame), nos dois sentidos
```
[cmd: 1 byte][len: 4 bytes little-endian][payload: len bytes]
```
O quadro pode chegar fatiado em várias escritas ou notificações: acumule até ter `5 + len` bytes. O payload tem no máximo 32 KB.

### Comandos
| Envia | Payload | Resposta |
|---|---|---|
| `0x01` GET_INFO | `[rede:1 (0=main, 1=test)][conta:4 LE]` | `0x81` + JSON (UTF-8) |
| `0x02` SIGN_PSBT | PSBT **binário** (não base64) | `0x90` status (texto, informativo), depois `0x82` PSBT assinado (binário) **ou** `0xFF` erro (texto) |
| `0x03` PING | vazio | `0x83` texto `PokeSigner 0.4.0 fp=xxxxxxxx` |
| `0x04` CONFIRM | 6 dígitos ASCII | nenhuma própria (resultado vem no `0x82`/`0xFF` do SIGN) |
| `0x05` CANCEL | vazio | idem (o SIGN volta `0xFF rejeitado no dispositivo`) |
| `0x06` NEXT / `0x07` PREV | vazio | `0x90 pagina N/M` |

**Aprovação por código (desde v0.4.0):** enquanto o SIGN_PSBT está pendente, o app pode mostrar botões
◀ ▶ ✖ e um campo "código da tela". A placa exibe na última página um **código de 6 dígitos aleatório** e aceita
`0x04` + código como aprovação (3 erros = rejeita). O app **não tem como saber o código**; o usuário lê na
placa e digita. É isso que mantém a segurança mesmo com o celular comprometido. Os `0x90` trazem
"pagina N/M", "codigo incorreto (k de 3)" etc. Mostre-os no diálogo.

- `0xFF` pode chegar em qualquer comando. Exemplos: `rejeitado no dispositivo`, `tempo esgotado`, `nenhuma entrada pertence a esta seed`, `derivacao nao confere com o script`, `UTXO ausente/inconsistente`, `ocupado`.
- No SIGN_PSBT o usuário revisa as páginas na placa e segura BOOT por 2 s. **Timeout da placa: 2 min.** Use timeout de leitura ≥ 3 min no app.
- Teste manual no nRF Connect: ative notify na `…0003` e escreva `0300000000` na `…0002`.

### JSON do GET_INFO
```json
{"fingerprint":"73c5da0a","network":"main","accounts":[
  {"type":"wpkh","path":"m/84h/0h/0h","xpub":"xpub6C...",
   "receive":"wpkh([73c5da0a/84h/0h/0h]xpub6C.../0/*)#checksum","change":"wpkh(...)/1/*)#checksum"},
  {"type":"tr","path":"m/86h/0h/0h","xpub":"xpub6B...","receive":"tr(...)#...","change":"tr(...)#..."}]}
```
Na testnet vem `tpub` e coin `1h`. Para a PokeWallet, monte a string de origem que `WalletWatchOnlyImport.parse()` já aceita:
```
"[" + fingerprint + "/" + path.removePrefix("m/") + "]" + xpub
// ex.: [73c5da0a/84h/0h/0h]xpub6C...
```
Com isso, `hasVerifiedFingerprint = true`, e o `PsbtAssembler` grava o fingerprint real. **Isso é obrigatório:** a placa só assina entradas com o fingerprint dela. PSBT com fingerprint zerado volta `nenhuma entrada pertence a esta seed`.

### O que a placa exige e confere no PSBT
(O `PsbtAssembler` atual já gera tudo isso.)
- PSBT v0. Cada entrada nossa precisa de `witness_utxo` (0x01).
  - P2WPKH: `PSBT_IN_BIP32_DERIVATION` (0x06).
  - P2TR: `PSBT_IN_TAP_BIP32_DERIVATION` (0x16, sem leaf hashes) + `PSBT_IN_TAP_INTERNAL_KEY` (0x17).
- Taproot: **todas** as entradas precisam de `witness_utxo` (exigência do BIP341).
- Troco é reconhecido por `PSBT_OUT_BIP32_DERIVATION` (0x02) ou `PSBT_OUT_TAP_BIP32_DERIVATION` (0x07) + derivação que confere com o script. Saída sem isso aparece como "destino".
- Sighash: ausente/ALL (P2WPKH); ausente/DEFAULT/ALL (P2TR).
- A placa devolve **o PSBT original + assinaturas**:
  - P2WPKH: `0x02` (key = 0x02‖pubkey33, value = DER‖0x01).
  - P2TR: `0x13` (value = assinatura Schnorr de 64 bytes; 65 se sighash 0x01).

---

## 3. Mudanças na PokeWallet

### 3.1 `crypto/PsbtTaproot.kt` (obrigatório)
O `parseBase64()` hoje **não lê** `PSBT_IN_TAP_KEY_SIG`. Adicione no `when` dos inputs:
```kotlin
0x13 -> input.tapKeySig = value
```
Atualize também o comentário que diz que o app nunca lê 0x13. O `Psbt.parseBase64()` (SegWit v0) já lê 0x02 e o `finalize()` já monta `[sig, pubkey]`.

### 3.2 Novo `crypto/PokeSignerPsbt.kt`
```kotlin
package com.pokewallet.crypto
import java.util.Base64

/** Finaliza o PSBT que voltou assinado do PokeSigner → hex da tx pronta. */
object PokeSignerPsbt {
    fun finalizeSigned(signed: ByteArray): String {
        val b64 = Base64.getEncoder().encodeToString(signed)
        val seg = Psbt.parseBase64(b64)
        val spk = seg.inputs.firstOrNull()?.witnessUtxo?.scriptPubKey
            ?: error("PSBT assinado sem witness_utxo")
        val isTaproot = spk.size == 34 && spk[0] == 0x51.toByte()
        val raw = if (isTaproot) PsbtTaproot.parseBase64(b64).finalize() else seg.finalize()
        return raw.toHex()
    }
}
```
A segurança contra troca de transação já existe: `submitSignedAirGappedTx()` compara o txid com o `expectedTxid` antes de transmitir.

### 3.3 Novo `android/PokeSignerClient.kt` (transporte BLE)
Esqueleto (corrotinas; `@SuppressLint("MissingPermission")`, porque as permissões são checadas antes):
```kotlin
@SuppressLint("MissingPermission")
class PokeSignerClient(private val ctx: Context) : Closeable {
    companion object {
        val SERVICE: UUID = UUID.fromString("50530001-b5a3-f393-e0a9-50ca5e1d0001")
        val RX: UUID      = UUID.fromString("50530002-b5a3-f393-e0a9-50ca5e1d0001")
        val TX: UUID      = UUID.fromString("50530003-b5a3-f393-e0a9-50ca5e1d0001")
        val CCCD: UUID    = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val CMD_GET_INFO = 0x01; const val CMD_SIGN = 0x02; const val CMD_PING = 0x03
        const val RSP_INFO = 0x81; const val RSP_SIGNED = 0x82; const val RSP_PONG = 0x83
        const val RSP_STATUS = 0x90; const val RSP_ERROR = 0xFF
    }
    class SignerException(msg: String) : Exception(msg)

    // estado: gatt, rxChar, mtu=23, frames = Channel<Pair<Int, ByteArray>>(UNLIMITED),
    // buffer acumulador, CompletableDeferred para connect / mtu / services / write / descriptor.

    suspend fun findDevice(timeoutMs: Long = 15_000): BluetoothDevice
    //  BluetoothManager.adapter (exigir isEnabled) → bluetoothLeScanner.startScan(
    //    listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()),
    //    ScanSettings.Builder().setScanMode(SCAN_MODE_LOW_LATENCY).build(), callback)
    //  primeiro resultado → stopScan → device. Timeout: "PokeSigner não encontrado…"

    suspend fun connect(device: BluetoothDevice)
    //  device.connectGatt(ctx, false, cb, BluetoothDevice.TRANSPORT_LE) → await CONNECTED
    //  requestMtu(517) → await onMtuChanged (ignora falha)
    //  discoverServices() → pega service/RX/TX
    //  se device.bondState != BOND_BONDED: createBond() (ou só aguardar se já estiver BONDING)
    //    e esperar ACTION_BOND_STATE_CHANGED == BOND_BONDED (timeout 90 s;
    //    BOND_NONE = "pareamento cancelado ou código errado")
    //  setCharacteristicNotification(tx, true) + escrever CCCD = ENABLE_NOTIFICATION_VALUE

    suspend fun request(cmd: Int, payload: ByteArray, timeoutMs: Long,
                        onStatus: (String) -> Unit = {}): ByteArray
    //  frame = byteArrayOf(cmd) + len(4 LE) + payload; fatiar em (mtu-3).coerceAtMost(512);
    //  cada fatia: write com resposta (WRITE_TYPE_DEFAULT), aguardando onCharacteristicWrite == GATT_SUCCESS
    //    API 33+: gatt.writeCharacteristic(ch, bytes, WRITE_TYPE_DEFAULT)
    //    <33: ch.writeType = DEFAULT; ch.value = bytes; gatt.writeCharacteristic(ch)
    //  depois: loop frames.receive(): 0x90 → onStatus(texto); 0xFF → throw SignerException(texto); senão retorna payload

    suspend fun ping(): String = String(request(CMD_PING, ByteArray(0), 10_000))
    suspend fun getInfo(testnet: Boolean, account: Int = 0): JSONObject =
        JSONObject(String(request(CMD_GET_INFO, byteArrayOf(if (testnet) 1 else 0) + le32(account), 15_000)))
    suspend fun signPsbt(psbt: ByteArray, onStatus: (String) -> Unit): ByteArray =
        request(CMD_SIGN, psbt, 200_000, onStatus)

    override fun close() { gatt?.disconnect(); gatt?.close() }

    // BluetoothGattCallback:
    //  onConnectionStateChange: CONNECTED → completa; DISCONNECTED → falha tudo que estiver pendente
    //    ("Conexão com o PokeSigner caiu") e fecha o channel
    //  onMtuChanged → mtu = value
    //  onCharacteristicChanged(gatt, ch, value) [API 33+] e a versão antiga (ch.value) só se SDK < 33
    //    → acumula no buffer; enquanto size >= 5 && size >= 5+len: emite (cmd, payload)
}
```
Observações Android:
- Escritas GATT são **uma por vez**: só envie a próxima depois do `onCharacteristicWrite`.
- Se a primeira escrita voltar `GATT_INSUFFICIENT_AUTHENTICATION`/`ENCRYPTION`, aguarde o bond e repita.
- Feche o GATT ao terminar (cada operação abre e fecha a conexão; é simples e robusto).

### 3.4 `AndroidManifest.xml`
```xml
<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation" tools:targetApi="s" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-feature android:name="android.hardware.bluetooth_le" android:required="false" />
```
Em runtime: Android 12+ pede `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`; Android ≤ 11 pede `ACCESS_FINE_LOCATION` (com a localização do sistema ligada para o scan funcionar).

### 3.5 `WalletViewModel.kt`
```kotlin
sealed class PokeSignerState {
    object Idle : PokeSignerState()
    data class Working(val message: String) : PokeSignerState()
    data class OriginReady(val origin: String) : PokeSignerState()   // para o import
    data class Error(val message: String) : PokeSignerState()
}
```
- `readPokeSignerOrigin(network, spendType)`: findDevice → connect → `getInfo(testnet = network != MAINNET)` → conta com `type == "wpkh"` (BIP84) ou `"tr"` (BIP86) → `OriginReady("[fp/path]xpub")`. Se o fingerprint da placa colidir com uma carteira com seed, o `importWatchOnly()` já trata (`ConflictWithKeyedWallet`).
- `signWithPokeSigner(psbt: AirGappedPsbt, mode: SendMode)` em `Dispatchers.IO`:
  1. `Working("Procurando PokeSigner…")` → findDevice
  2. `Working("Conectando… se pedir, digite o código da tela da placa")` → connect
  3. `Working("Revise na placa: BOOT avança, segure 2 s na última tela para aprovar")` → `signPsbt(Base64.decode(psbt.psbtBase64))`
  4. `PokeSignerPsbt.finalizeSigned(signed)` → `submitSignedAirGappedTx(raw, psbt.expectedTxid, psbt.network, mode)`. O estado de broadcast que já existe mostra sucesso ou erro.
  5. Em erro: `Error(humanizeError(e))`. Sempre `client.close()`.

### 3.6 Interface
- `dialog_airgapped_psbt.xml` + `showAirGappedPsbtDialog()`: botão grande **"🎮 Assinar no PokeSigner (Bluetooth)"** acima do QR, e um `TextView` de status ligado ao `pokeSignerState`. Use o `broadcastMode` escolhido no RadioGroup que já existe.
- `dialog_watchonly_import.xml` + `WalletCreationFlow.showWatchOnlyImportDialog()`: botão **"🎮 Ler do PokeSigner"** perto do campo `et_account_origin`. Ele usa a rede e o tipo escolhidos nos chips; em `OriginReady`, preenche o campo. O usuário confirma com o botão Importar que já existe.
- Antes de qualquer operação: checar permissões e Bluetooth ligado, com mensagens em PT-BR no tom do app.

### 3.7 Testes sugeridos
- Unitário de `PokeSignerPsbt.finalizeSigned()` para P2WPKH e P2TR: PSBT do `PsbtAssembler` → assinatura falsa inserida → tx final com o witness certo e txid igual ao `expectedTxid`.
- Manual: carteira testnet importada do PokeSigner → receber tBTC → enviar → aprovar na placa → conferir no mempool.space/testnet.

---

## 4. Erros comuns
| Sintoma | Causa |
|---|---|
| Não acha no scan | placa ainda sem seed (o BLE só liga depois da configuração), Bluetooth/localização desligados, permissão negada |
| `nenhuma entrada pertence a esta seed` | carteira importada por xpub pura (fingerprint zerado), outra seed/passphrase na placa, ou rede errada |
| `UTXO ausente/inconsistente` | Taproot com alguma entrada sem `witness_utxo` |
| `tempo esgotado` | ninguém aprovou na placa em 2 min, ou a conexão caiu |
| Pede código toda vez | o bond foi apagado na placa (`unpair`/`wipe`): esqueça o aparelho no Android e pareie de novo |
