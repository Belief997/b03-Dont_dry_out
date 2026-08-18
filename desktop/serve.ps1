<#
  serve.ps1 —— 启动本地 HTTP 服务并打开上位机页面
  ------------------------------------------------------------------
  为什么必须走 HTTP 而不能直接双击 index.html:

    页面的 BLE 模式依赖 Web Bluetooth, 而该 API 只在【安全上下文】
    (secure context) 下暴露。file:// 不是安全上下文 —— 双击打开时
    navigator.bluetooth 直接是 undefined, 扫描和连接都用不了。
    localhost 被浏览器视作安全来源(等同 https), 所以起一个本地
    http server 就够了, 不需要自签证书。

    串口模式(Web Serial) 同样要求安全上下文, 所以这个脚本对两种
    模式都是必需的。

  ⚠ 另一个独立的开关(本脚本管不到):
    BLE【扫描】用的 navigator.bluetooth.requestLEScan() 至今仍是实验
    API, 需要在 chrome://flags/#enable-experimental-web-platform-features
    里开启并重启浏览器。「连接设备」不需要这个开关, 只有扫描需要。

  用法:
      .\serve.ps1                 # 默认 8000 端口, 自动开浏览器
      .\serve.ps1 -Port 8080      # 换端口(8000 被占用时)
      .\serve.ps1 -NoBrowser      # 只起服务, 不自动开浏览器
      Ctrl+C                      # 停止服务
#>

[CmdletBinding()]
param(
    [int]    $Port = 8000,
    [switch] $NoBrowser
)

$ErrorActionPreference = 'Stop'

# 以脚本自身所在目录为站点根 —— 这样从任何工作目录调用都能正确服务 index.html
$root = $PSScriptRoot
if (-not (Test-Path (Join-Path $root 'index.html'))) {
    Write-Error "未找到 index.html: $root`n这个脚本必须和 index.html 放在同一目录。"
}

# 找一个可用的 Python。
# ⚠ python.exe 优先于 py.exe(launcher): py 只是个转发器, 会把真正的 python.exe
#   再起成一个子进程 —— 于是本脚本 → py.exe → python.exe 中间多一层。实测这一层
#   让端口 bind 明显变慢(3 秒时还没监听, 8 秒才起来), 而直接调 python.exe 是即时的;
#   而且 Ctrl+C 时那个孙进程可能被留下, 端口不释放。所以宁可要直连。
$py = $null
foreach ($cand in 'python', 'python3', 'py') {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    # Windows 的 App Execution Alias: 未安装 Python 时 PATH 里有个 0 字节的
    # python.exe 占位, 点开只会跳转应用商店。用文件大小把它筛掉。
    if ($cmd -and $cmd.Source -and (Get-Item $cmd.Source).Length -gt 0) { $py = $cmd.Source; break }
}
if (-not $py) {
    Write-Error "未找到 Python。请安装 Python 3 或把它加入 PATH。"
}

# 端口占用检查: 否则 python 会抛 WinError 10048, 而错误信息里不会提示换端口
$busy = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($busy) {
    $owner = (Get-Process -Id $busy[0].OwningProcess -ErrorAction SilentlyContinue).ProcessName
    Write-Error "端口 $Port 已被占用(进程: $owner)。换个端口: .\serve.ps1 -Port 8080"
}

$url = "http://localhost:$Port/index.html"

Write-Host ''
Write-Host "  站点根 : $root"
Write-Host "  地址   : $url" -ForegroundColor Cyan
Write-Host "  Python : $py"
Write-Host ''
Write-Host '  BLE 扫描还需在 chrome://flags/#enable-experimental-web-platform-features' -ForegroundColor DarkYellow
Write-Host '  开启实验特性并重启浏览器(「连接设备」不需要)。' -ForegroundColor DarkYellow
Write-Host ''
Write-Host '  Ctrl+C 停止服务'
Write-Host ''

if (-not $NoBrowser) {
    # 先排好开浏览器的动作, 再阻塞在 server 上。延迟是为了让 server 先 bind 端口,
    # 否则浏览器可能抢在监听之前请求, 拿到连接被拒。
    Start-Job -ScriptBlock {
        Start-Sleep -Milliseconds 700
        Start-Process $using:url
    } | Out-Null
}

# --directory 让 python 服务指定目录, 不必 cd 过去(Python 3.7+)
# --bind 127.0.0.1 只监听本机: 局域网内其它机器访问不到, 免得把调试页面暴露出去。
# 这里是前台阻塞运行, Ctrl+C 直接停掉。
& $py -m http.server $Port --bind 127.0.0.1 --directory $root
