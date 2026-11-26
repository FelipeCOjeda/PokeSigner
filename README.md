# PokeSigner — assinador de PSBT via BLE para ESP32-C6

Firmware ESP-IDF que transforma um ESP32-C6 em assinador Bitcoin single-sig.
O celular (PokeWallet) envia o PSBT por BLE; o dispositivo mostra os detalhes
**na tela** (Waveshare ESP32-C6-LCD-1.47) e só assina após você revisar todas as
páginas e **segurar o botão BOOT por 2 s**.

## Para desenvolvedores de carteiras

Quer suportar o Signy na sua carteira? O protocolo completo, aberto e com exemplos (Web Bluetooth, Android, iOS, Python), está em **[SIGNY-PROTOCOL.md](SIGNY-PROTOCOL.md)**.

## Instalação plug-and-play

Pasta `installer/`: clique duas vezes em `Instalar-Windows.bat`, `Instalar-Mac.command`
ou rode `Instalar-Linux.sh`. Abre uma página no Chrome/Edge que grava o firmware pela USB
(Web Serial) e um terminal opcional de diagnóstico. Detalhes em `installer/LEIA-ME.txt`.
A mesma pasta pode ser publicada no GitHub Pages para instalar só com um link.

## O que suporta

| Item | Status |
|---|---|
| BIP39 (12–24 palavras) + passphrase | ✅ |
| P2WPKH — BIP84 (`m/84h/0h/0h`) | ✅ ECDSA, SIGHASH_ALL |
| P2TR key-path — BIP86 (`m/86h/0h/0h`) | ✅ Schnorr, SIGHASH_DEFAULT/ALL |
| PSBT v0 (BIP174) | ✅ |
| `non_witness_utxo` (anti-ataque de taxa em segwit v0) | ✅ verificado se presente |
| Verificação de troco (derivação + script) | ✅ |
| Mainnet / testnet (pelo coin type do caminho) | ✅ |
| PSBT v2, multisig, script-path taproot, P2SH-P2WPKH, legacy | ❌ recusa |

## Hardware

Duas placas suportadas (o instalador pergunta qual é a sua):

| Placa | Tela | Botões |
|---|---|---|
| **Waveshare ESP32-C6-LCD-1.47** | ST7789 172×320 (MOSI 6, SCLK 7, CS 14, DC 15, RST 21, BL 22), grade 20×10 | BOOT (GPIO9) |
| **Spotpear ESP32-C3 LCD 1.44** | ST7735S 128×128 (MOSI 4, SCLK 3, CS 2, DC 0, RST 5), grade 16×16 | BOOT (GPIO9) e GPIO8 avançam/aprovam; GPIO10 rejeita |
| **LilyGO T-Display ESP32 1.14** | ST7789V 240×135 (MOSI 19, SCLK 18, CS 5, DC 16, RST 23, BL 4), grade 20×11 com fonte em 1,5× | GPIO35 avança; GPIO0 (BOOT) avança/aprova segurando 2 s |

Compilar para cada uma:
```bash
idf.py build                                                     # ESP32-C6 → build/
idf.py -B build_c3 -D SDKCONFIG=sdkconfig.c3 set-target esp32c3  # 1ª vez
idf.py -B build_c3 -D SDKCONFIG=sdkconfig.c3 build               # ESP32-C3 → build_c3/
idf.py -B build_esp32 -D SDKCONFIG=sdkconfig.esp32 set-target esp32  # 1ª vez (T-Display)
idf.py -B build_esp32 -D SDKCONFIG=sdkconfig.esp32 build         # ESP32 T-Display → build_esp32/
```

- Outras placas ESP32-C6: desative a tela em `idf.py menuconfig` → *PokeSigner* (aí tudo vai pelo serial).
- Botão: **BOOT = GPIO9**.
- Console: **USB nativo (USB-Serial-JTAG)**.
- Se as cores ou a posição da imagem vierem erradas: `menuconfig` → *PokeSigner* → inverter cores / ordem BGR / offset X.

## Mascote

**Signy**, um robozinho portátil em pixel art (original), aparece ao ligar, fica na tela de espera piscando e comemora pulando quando um PSBT é assinado.

![Signy](installer/assets/signy-c6.gif)

## Uso na tela

Visual inspirado nos portáteis 8-bit: tela na horizontal, grade de 20×10 blocos, fonte pixel
(Press Start 2P, OFL) e caixas de diálogo. Se a imagem ficar invertida, use `girar` no terminal do instalador.


| Tela | Botão BOOT |
|---|---|
| Revisão (resumo → cada destino → troco → confirmar) | toque = próxima página |
| Última página "CONFIRMAR?" | **segure 2 s = aprovar**, toque = rejeitar |
| Seed gerada (2 páginas de 12 palavras) | toque = próxima |

O código de pareamento BLE aparece grande na tela. Sem aprovação em 2 min, a transação é cancelada.

## Compilar e gravar

```bash
# ESP-IDF v5.4.x instalado e exportado (. $IDF_PATH/export.sh)
cd pokesigner
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Ou gravar a imagem pronta (pasta `installer/firmware/<placa>/`):

```bash
python -m esptool --chip esp32c6 write_flash 0x0 installer/firmware/c6/pokesigner-esp32c6.bin
python -m esptool --chip esp32c3 write_flash 0x0 installer/firmware/c3/pokesigner-esp32c3.bin
```

## Primeiro uso

A seed é configurada **só no dispositivo** (tela + botões); o computador não participa.
Toque curto = próxima opção, segurar ~0,7 s = escolher (no C3 o GPIO10 volta uma opção).

1. **GERAR**: 24 palavras do TRNG do chip, mostradas só na tela com o número BIP39 (1–2048);
   depois você escolhe na tela 3 palavras pedidas (entre 4 opções).
2. **DADOS**: informe lançamentos de um dado d6 (mín. 20, recomendado 99). Entropia =
   SHA-256(TRNG ‖ dados ‖ tempo), a mesma receita da PokeWallet.
3. **IMPORTAR**: 12/24 palavras pelo número na lista oficial (1–2048 = linha do `english.txt`) ou
   letra a letra (só letras que formam palavras válidas); checksum validado, com correção.
4. **PASSPHRASE**: nenhuma ou digitada nos botões (ex. `pokemon:74:Geodude`), com revisão.
5. O dispositivo mostra o **fingerprint** e começa a anunciar `PokeSigner-XXXX`.

Placas sem tela continuam com a configuração pelo terminal serial.

Comandos no serial depois de iniciado: `info` (descritores), `scan`/`mac` (teste do Bluetooth), `potencia`, `unpair` (apaga
pareamentos BLE), `wipe` (apaga seed + pareamentos; exige segurar BOOT 5 s).

## Pareamento BLE

- LE Secure Connections com MITM (passkey). Pareamento legado desativado.
- O código de 6 dígitos aparece **na tela da placa** (e no serial); digite-o no celular.
- Só conexões autenticadas conseguem escrever na característica RX.

## Protocolo

Serviço GATT `50530001-b5a3-f393-e0a9-50ca5e1d0001`

| Característica | UUID | Direção |
|---|---|---|
| RX | `50530002-b5a3-f393-e0a9-50ca5e1d0001` | write / write-without-response (celular → signer) |
| TX | `50530003-b5a3-f393-e0a9-50ca5e1d0001` | notify (signer → celular) |

Cada mensagem: `[cmd:1][len:4 little-endian][payload]`, fatiada livremente em
quantas escritas/notificações forem necessárias (o MTU é negociado até 517).
Payload máximo: 32 KB.

| Requisição | Payload | Resposta |
|---|---|---|
| `0x01` GET_INFO | `[rede:1 (0 main, 1 test)][conta:4 LE]` (opcionais) | `0x81` JSON |
| `0x02` SIGN_PSBT | PSBT **binário** | `0x90` status, depois `0x82` PSBT assinado ou `0xFF` erro |
| `0x03` PING | — | `0x83` texto |
| `0x04` CONFIRM | 6 dígitos ASCII (o código da tela) | — (aprova se estiver na última página e o código bater) |
| `0x05` CANCEL | — | — (rejeita) |
| `0x06` NEXT / `0x07` PREV | — | — (muda a página da revisão) |

### Aprovação por código (estilo token)

Na última página da revisão a placa mostra um **código aleatório de 6 dígitos**, novo a cada transação.
O usuário pode aprovar segurando BOOT **ou** digitando esse código no celular (`0x04`). Um app comprometido
não vê a tela da placa, então não consegue aprovar sozinho. Cancelar e navegar pelas páginas pelo celular são
livres. O `CONFIRM` só vale na última página, e 3 códigos errados rejeitam a transação. Durante a revisão, a placa
envia `0x90` com a página atual e o resultado de cada tentativa. Os comandos 0x04–0x07 são aceitos **enquanto um
SIGN_PSBT está pendente**. Sem tela, só o botão aprova.

Resposta de GET_INFO:

```json
{"fingerprint":"73c5da0a","network":"main","accounts":[
 {"type":"wpkh","path":"m/84h/0h/0h","xpub":"xpub6C...","receive":"wpkh([73c5da0a/84h/0h/0h]xpub6C.../0/*)#...","change":"...#..."},
 {"type":"tr","path":"m/86h/0h/0h","xpub":"xpub6B...","receive":"tr([...])#...","change":"...#..."}]}
```

O PSBT devolvido é o original com `PSBT_IN_PARTIAL_SIG` (P2WPKH) ou
`PSBT_IN_TAP_KEY_SIG` (P2TR) adicionados. Finalizar e transmitir fica a cargo da wallet.

### O que a PokeWallet precisa colocar no PSBT

Para cada entrada a assinar:
- `PSBT_IN_WITNESS_UTXO` (e, se possível, `PSBT_IN_NON_WITNESS_UTXO` para P2WPKH);
- P2WPKH: `PSBT_IN_BIP32_DERIVATION` (pubkey → fingerprint + caminho);
- P2TR: `PSBT_IN_TAP_BIP32_DERIVATION` (x-only, 0 leaf hashes) e `PSBT_IN_TAP_INTERNAL_KEY`.
- Se houver entrada taproot, **todas** as entradas precisam do UTXO (exigência do BIP341).

Para o troco ser reconhecido (e não aparecer como "destino"):
- `PSBT_OUT_BIP32_DERIVATION` (P2WPKH) ou `PSBT_OUT_TAP_BIP32_DERIVATION` + `PSBT_OUT_TAP_INTERNAL_KEY` (P2TR).

### Esboço em Kotlin (framing)

```kotlin
fun frame(cmd: Int, payload: ByteArray): ByteArray =
    ByteBuffer.allocate(5 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
        .put(cmd.toByte()).putInt(payload.size).put(payload).array()

// envio: fatiar frame(...) em pedaços de (mtu - 3) e escrever na RX
// recepção: acumular notificações da TX até ter 5 + len bytes
```

## Teste pelo PC

```bash
pip install bleak
python3 tools/client.py ping
python3 tools/client.py info --testnet
python3 tools/client.py sign tx.psbt -o tx-assinado.psbt
```

Testes do núcleo (rodam no PC, comparam com a biblioteca `embit`):

```bash
cd host_test && pip install embit && ./build.sh && python3 test.py && python3 fuzz.py
# t_screens.c renderiza as telas em PPM para conferir o layout no PC
```

## Segurança — leia antes de colocar valor relevante

- A tela mostra tudo que é assinado; o computador só é necessário na configuração inicial.
- **BLE não é air-gap**: a superfície de ataque inclui a pilha Bluetooth.
- **Seed na flash sem criptografia por padrão.** Para proteger contra leitura física, ative
  Flash Encryption + NVS Encryption + Secure Boot v2 (`idf.py menuconfig` → Security features).
  Em modo *Release* isso é **irreversível** e queima eFuses; teste antes em modo *Development*.
- ESP32 não tem secure element; ataques de fault injection em chips ESP32 já foram publicados.
- Use para estudo e valores pequenos.
