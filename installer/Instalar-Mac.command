#!/bin/sh
# PokeSigner - abre o instalador web em http://localhost:8765
cd "$(dirname "$0")" || exit 1
command -v python3 >/dev/null 2>&1 || { echo "Instale o python3: abra o Terminal e rode  xcode-select --install"; exit 1; }
# Linux: o usuario precisa de acesso a porta serial
if [ "$(uname)" = "Linux" ] && ! id -nG | grep -qw dialout; then
  echo "AVISO: seu usuario nao esta no grupo 'dialout'. Se a placa nao aparecer, rode:"
  echo "  sudo usermod -aG dialout $USER   (e faca logout/login)"
fi
URL=http://localhost:8765/
echo "PokeSigner - instalador em $URL  (Ctrl+C para encerrar)"
( sleep 1; for b in open; do
    open -a "Google Chrome" "$URL" 2>/dev/null || open -a "Microsoft Edge" "$URL" 2>/dev/null || open "$URL"; done ) &
exec python3 -m http.server 8765 --bind 127.0.0.1
