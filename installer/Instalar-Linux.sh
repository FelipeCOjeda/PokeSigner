#!/bin/sh
# PokeSigner - abre o instalador web em http://localhost:8765
cd "$(dirname "$0")" || exit 1
command -v python3 >/dev/null 2>&1 || { echo "Instale o python3 (sudo apt install python3)"; exit 1; }
# Linux: o usuario precisa de acesso a porta serial
if [ "$(uname)" = "Linux" ] && ! id -nG | grep -qw dialout; then
  echo "AVISO: seu usuario nao esta no grupo 'dialout'. Se a placa nao aparecer, rode:"
  echo "  sudo usermod -aG dialout $USER   (e faca logout/login)"
fi
URL=http://localhost:8765/
echo "PokeSigner - instalador em $URL  (Ctrl+C para encerrar)"
( sleep 1; for b in google-chrome google-chrome-stable chromium chromium-browser microsoft-edge xdg-open open; do
    command -v $b >/dev/null 2>&1 && { $b "$URL" >/dev/null 2>&1 & break; }; done ) &
exec python3 -m http.server 8765 --bind 127.0.0.1
