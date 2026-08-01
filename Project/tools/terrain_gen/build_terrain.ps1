# ============================================================
#  build_terrain.ps1 — Gaea Swarm 빌드 실행기
#
#  ★ Gaea.Swarm 은 진짜 콘솔 창이 없으면 죽는다.
#    (Spectre.Console 이 콘솔 핸들을 요구 → 파이프/파일 리다이렉트 모두 IOException)
#    그래서 Start-Process 로 자체 콘솔을 주고, 출력은 로그 파일에서 읽는다.
# ============================================================
param(
    [Parameter(Mandatory = $true)][string]$Terrain,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Resolution = 0,
    [int]$Seed = -1,
    [string]$GaeaDir = "V:\User\Gaea 2.0",
    [switch]$Quiet
)

New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -Force

$before = Get-ChildItem "$GaeaDir\Data\Logs" -Filter "*SWARM*" -ErrorAction SilentlyContinue |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1

$a = '--filename "' + $Terrain + '" --buildpath "' + $OutDir + '" --silent'
if ($Resolution -gt 0) { $a += " --resolution $Resolution" }
if ($Seed -ge 0) { $a += " --seed $Seed" }

$p = Start-Process -FilePath "$GaeaDir\Gaea.Swarm.exe" -ArgumentList $a `
        -WorkingDirectory $GaeaDir -Wait -PassThru

$files = @(Get-ChildItem $OutDir -Recurse -File -ErrorAction SilentlyContinue)
$log = Get-ChildItem "$GaeaDir\Data\Logs" -Filter "*SWARM*" -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1

$result = [PSCustomObject]@{
    Name    = [IO.Path]::GetFileNameWithoutExtension($Terrain)
    Ok      = ($files.Count -gt 0)
    Exit    = $p.ExitCode
    Files   = $files.Count
    Log     = if ($log -and (-not $before -or $log.FullName -ne $before.FullName)) { $log.FullName } else { $null }
}

if (-not $Quiet) {
    "{0,-24} {1}  exit={2}  files={3}" -f $result.Name, $(if ($result.Ok) { "OK  " } else { "FAIL" }), $result.Exit, $result.Files
    if (-not $result.Ok -and $result.Log) {
        (Get-Content $result.Log -Raw) -split "`n" | Select-Object -Last 4 | ForEach-Object { "    $_" }
    }
}
$result
