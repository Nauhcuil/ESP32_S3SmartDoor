# 小程序直连版服务层冒烟（无需安装依赖）
# 用法：powershell -ExecutionPolicy Bypass -File .\run.ps1
#      可传 -Smartdoor 指向其它小程序工程目录
param(
    [string]$Smartdoor = 'D:\Develop\miniprogram\WeChatProjects\Smartdoor'
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '.work'

if (-not (Test-Path -LiteralPath (Join-Path $Smartdoor 'services\api.js'))) {
    throw "找不到小程序服务层：$Smartdoor\services\api.js"
}

if (Test-Path -LiteralPath $work) {
    Remove-Item -LiteralPath $work -Recurse -Force
}
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item -LiteralPath (Join-Path $Smartdoor 'services') -Destination (Join-Path $work 'services') -Recurse
'{ "type": "module" }' | Set-Content -LiteralPath (Join-Path $work 'package.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'smoke-protocol.mjs') -Destination (Join-Path $work 'smoke-protocol.mjs')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'smoke-api.mjs') -Destination (Join-Path $work 'smoke-api.mjs')

Push-Location -LiteralPath $work
try {
    Write-Output '=== 协议层 / 业务层（假 transport） ==='
    & node smoke-protocol.mjs
    if ($LASTEXITCODE -ne 0) { throw "smoke-protocol.mjs 失败（exit $LASTEXITCODE）" }

    Write-Output ''
    Write-Output '=== api / bemfa / config（伪 wx.request） ==='
    & node smoke-api.mjs
    if ($LASTEXITCODE -ne 0) { throw "smoke-api.mjs 失败（exit $LASTEXITCODE）" }
}
finally {
    Pop-Location
}

Write-Output ''
Write-Output '全部冒烟通过。'
