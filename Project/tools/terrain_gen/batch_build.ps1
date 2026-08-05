# ============================================================
#  batch_build.ps1 — .terrain 폴더를 통째로 빌드하고 성공/실패를 표로 낸다
#
#  ★ Gaea.Swarm 은 진짜 콘솔이 필요하다 (build_terrain.ps1 주석 참고)
# ============================================================
param(
    [Parameter(Mandatory = $true)][string]$SrcDir,
    [Parameter(Mandatory = $true)][string]$OutRoot,
    [int]$Resolution = 0,
    [int]$Seed = -1,
    [string]$GaeaDir = "V:\User\Gaea 2.0"
)

$files = @(Get-ChildItem $SrcDir -Filter *.terrain | Sort-Object Name)
"대상 $($files.Count) 개"
New-Item -ItemType Directory -Force $OutRoot | Out-Null

$ok = @(); $fail = @()
$i = 0
foreach ($f in $files) {
    $i++
    $name = $f.BaseName
    $od = Join-Path $OutRoot $name
    New-Item -ItemType Directory -Force $od | Out-Null

    $a = '--filename "' + $f.FullName + '" --buildpath "' + $od + '" --silent'
    if ($Resolution -gt 0) { $a += " --resolution $Resolution" }
    if ($Seed -ge 0) { $a += " --seed $Seed" }

    # 콘솔은 필요하지만 창이 계속 튀어나오면 방해되므로 최소화해서 띄운다
    $p = Start-Process -FilePath "$GaeaDir\Gaea.Swarm.exe" -ArgumentList $a `
            -WorkingDirectory $GaeaDir -Wait -PassThru -WindowStyle Minimized
    $produced = @(Get-ChildItem $od -Recurse -File -Filter *.png -ErrorAction SilentlyContinue)

    if ($produced.Count -gt 0) {
        $ok += $name
        "[{0,3}/{1}] OK    {2}" -f $i, $files.Count, $name
    } else {
        $fail += $name
        Remove-Item $od -Recurse -Force -ErrorAction SilentlyContinue
        "[{0,3}/{1}] fail  {2}" -f $i, $files.Count, $name
    }
}

""
"=== 성공 $($ok.Count) / 실패 $($fail.Count) ==="
if ($fail.Count) { "실패: " + ($fail -join ", ") }
