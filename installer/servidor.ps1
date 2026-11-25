# Servidor local minimo para o instalador PokeSigner (Web Serial exige localhost ou HTTPS)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = 8765
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
try { $listener.Start() } catch { Write-Host "Nao consegui abrir a porta $port. Feche outra janela do instalador e tente de novo."; Read-Host "ENTER para sair"; exit 1 }
$url = "http://localhost:$port/"
Write-Host ""
Write-Host "  PokeSigner - instalador rodando em $url"
Write-Host "  Abra no Chrome ou no Edge. Feche esta janela quando terminar."
Write-Host ""
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$chrome = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe"
if (Test-Path $chrome) { Start-Process $chrome $url } elseif (Test-Path $edge) { Start-Process $edge $url } else { Start-Process $url }
$types = @{ ".html"="text/html; charset=utf-8"; ".json"="application/json"; ".bin"="application/octet-stream"; ".js"="text/javascript"; ".txt"="text/plain; charset=utf-8" }
while ($listener.IsListening) {
  $ctx = $listener.GetContext()
  $rel = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
  if ($rel -eq "") { $rel = "index.html" }
  $path = [IO.Path]::GetFullPath((Join-Path $root $rel))
  $resp = $ctx.Response
  if ($path.StartsWith($root) -and (Test-Path $path -PathType Leaf)) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $ext = [IO.Path]::GetExtension($path).ToLower()
    $resp.ContentType = $(if ($types.ContainsKey($ext)) { $types[$ext] } else { "application/octet-stream" })
    $resp.ContentLength64 = $bytes.Length
    $resp.OutputStream.Write($bytes, 0, $bytes.Length)
  } else { $resp.StatusCode = 404 }
  $resp.Close()
}
