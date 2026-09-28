<#
.SYNOPSIS
Tunes engine parameters by SPSA, with fastchess playing the games.

.DESCRIPTION
Simultaneous perturbation stochastic approximation, as on OpenBench. Each
iteration perturbs every parameter at once by +-c (the signs at random),
plays a few game pairs between the plus and the minus engine, and moves
the parameters towards whichever side scored better, by a step that
shrinks as the tune goes on.

Several fastchess processes run at once (-Workers), each playing one
iteration; the parameters are updated as each finishes and the next
iteration starts from the latest values, so no worker waits for the
slowest game of a big batch.

The parameters come from a CSV file with a header line and these columns:
  name   the UCI option (every entry in params.h tunables())
  value  the starting value
  min, max  the range it stays in
  c_end  the perturbation at the end of the tune, in the parameter's units.
         Big enough that +-c_end changes play measurably, small enough to
         stay near the optimum: often a twentieth of a sensible range.
  r_end  the learning rate at the end (OpenBench's default is 0.002).
The step sizes follow the standard schedules: c_k = c / k^0.101 and
a_k = a / (A + k)^0.602, with A a tenth of the iterations, scaled so that
they end at c_end and r_end * c_end^2.

Progress goes to a state CSV (-State): one line per finished iteration,
with its result and the parameter values after it. Running the same
command again resumes from the last line.

The engine needs a build configured with -DTUNE=ON: fastchess silently
skips options an engine doesn't advertise, which would make the tune a
long random walk. The script checks this.

.EXAMPLE
# Tune the double-extension margin and cap, 1000 iterations of 8 pairs.
tools\spsa.ps1 -Engine build-tune\chess.exe -Params tools\spsa\se-double.csv -Iterations 1000 -Book books\UHO_Lichess_4852_v1.epd -State matches\spsa-se-double.csv
#>
param(
    [Parameter(Mandatory)] [string] $Engine,
    [Parameter(Mandatory)] [string] $Params,
    [Parameter(Mandatory)] [int] $Iterations,
    [Parameter(Mandatory)] [string] $Book,
    [Parameter(Mandatory)] [string] $State,
    [string] $Fastchess = 'tools\fastchess\fastchess-windows-x86-64\fastchess.exe',
    [string] $TC = '8+0.08',
    [int] $Hash = 16,
    # Game pairs per iteration (each opening played with both colours).
    [int] $Pairs = 8,
    # Parallel fastchess processes, and games at once in each: 6 x 4 fills
    # the 24 threads of the test machine.
    [int] $Workers = 6,
    [int] $Concurrency = 4,
    # Options set to fixed values in both engines, as Name=Value strings:
    # a switch the tuned parameters depend on, say.
    [string[]] $Options = @()
)

$ErrorActionPreference = 'Stop'

foreach ($f in @($Engine, $Params, $Book, $Fastchess)) {
    if (-not (Test-Path $f)) { throw "Not found: $f" }
}
$enginePath = (Resolve-Path $Engine).Path
$bookPath = (Resolve-Path $Book).Path
$fastchessPath = (Resolve-Path $Fastchess).Path
$format = if ($Book -match '\.pgn$') { 'pgn' } else { 'epd' }

$spec = @(Import-Csv $Params)
if (-not $spec) { throw "No parameters in $Params" }
$names = @($spec | ForEach-Object { $_.name })

# fastchess drops options the engine doesn't advertise: refuse to start.
$advertised = (@('uci', 'quit') | & $enginePath) |
    Where-Object { $_ -match '^option name (\S+)' } | ForEach-Object { $Matches[1] }
foreach ($n in $names + @($Options | ForEach-Object { ($_ -split "=", 2)[0] })) {
    if ($advertised -notcontains $n) {
        throw "$Engine doesn't advertise option '$n'. Use a build configured with -DTUNE=ON."
    }
}

# Gain schedules (OpenBench's): c_k = c0 / k^gamma, a_k = a0 / (A + k)^alpha,
# scaled so that the last iteration has c_end and a_end = r_end * c_end^2.
$alpha = 0.602
$gamma = 0.101
$bigA = 0.1 * $Iterations
$p = @{}
foreach ($s in $spec) {
    $cEnd = [double]$s.c_end
    $aEnd = [double]$s.r_end * $cEnd * $cEnd
    $p[$s.name] = [pscustomobject]@{
        Min = [double]$s.min
        Max = [double]$s.max
        C0 = $cEnd * [Math]::Pow($Iterations, $gamma)
        A0 = $aEnd * [Math]::Pow($bigA + $Iterations, $alpha)
    }
}

function Clamp([double] $v, $q) { [Math]::Min([Math]::Max($v, $q.Min), $q.Max) }

# Rounds to one of the two nearest integers, the upper one with probability
# equal to the fractional part (2.3: 3 with 30%). Rounding to the nearest
# would often give the plus and minus engines the same value for a small
# integer parameter, and no signal; this way the average over iterations
# moves smoothly with theta, so every integer parameter has a gradient.
function Round-Stochastic([double] $v) {
    $floor = [Math]::Floor($v)
    if ((Get-Random -Minimum 0.0 -Maximum 1.0) -lt $v - $floor) { $floor + 1 } else { $floor }
}

# Current values, from the state file when resuming.
$theta = @{}
$done = 0
if (Test-Path $State) {
    $rows = @(Import-Csv $State)
    if ($rows) {
        $last = $rows[-1]
        foreach ($n in $names) { $theta[$n] = [double]$last.$n }
        $done = $rows.Count  # iterations finish out of order: count them
        Write-Host "Resuming after iteration $done of $Iterations"
    }
}
if (-not $theta.Count) {
    foreach ($s in $spec) { $theta[$s.name] = [double]$s.value }
    'iter,wins,losses,draws,' + ($names -join ',') | Set-Content -Encoding ascii $State
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("spsa-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null

# Starts iteration k: perturbs theta, launches fastchess, returns the job.
function Start-Iteration([int] $k) {
    $ck = @{}
    $delta = @{}
    $plus = @()
    $minus = @()
    foreach ($n in $names) {
        $q = $p[$n]
        $ck[$n] = $q.C0 / [Math]::Pow($k, $gamma)
        $delta[$n] = if ((Get-Random -Maximum 2) -eq 0) { -1 } else { 1 }
        $vp = Round-Stochastic (Clamp ($theta[$n] + $ck[$n] * $delta[$n]) $q)
        $vm = Round-Stochastic (Clamp ($theta[$n] - $ck[$n] * $delta[$n]) $q)
        $plus += "option.$n=$vp"
        $minus += "option.$n=$vm"
    }
    $fcArgs = @('-engine', "cmd=$enginePath", 'name=plus') + $plus +
        @('-engine', "cmd=$enginePath", 'name=minus') + $minus + @(
        '-each', "tc=$TC", "option.Hash=$Hash") + @($Options | ForEach-Object { "option.$_" }) + @(
        '-openings', "file=$bookPath", "format=$format", 'order=random',
        '-srand', (Get-Random -Maximum 2000000000),
        '-rounds', $Pairs, '-games', 2, '-repeat',
        '-concurrency', $Concurrency,
        '-draw', 'movenumber=40', 'movecount=8', 'score=10',
        '-resign', 'movecount=3', 'score=600',
        '-ratinginterval', 0,
        # fastchess saves its state to config.json in the working directory
        # by default; every worker needs its own file.
        '-config', "outname=$(Join-Path $tmp "$k.json")")
    $out = Join-Path $tmp "$k.out"
    # Start-Process joins the arguments with spaces (PowerShell 5.1): quote
    # any with a space in it (the temp path does, under a user name with one).
    $quoted = @($fcArgs | ForEach-Object { if ("$_" -match " ") { "`"$_`"" } else { "$_" } })
    $proc = Start-Process -FilePath $fastchessPath -ArgumentList $quoted -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError (Join-Path $tmp "$k.err")
    [pscustomobject]@{ K = $k; Proc = $proc; Out = $out; Ck = $ck; Delta = $delta }
}

$next = $done + 1
$running = @()
$failures = 0  # consecutive iterations without a result
$finished = $false
try {
    while ($done -lt $Iterations) {
        while ($running.Count -lt $Workers -and $next -le $Iterations) {
            $running += Start-Iteration $next
            $next++
        }
        Start-Sleep -Milliseconds 500
        foreach ($job in @($running | Where-Object { $_.Proc.HasExited })) {
            $running = @($running | Where-Object { $_ -ne $job })
            # fastchess reports the first engine's (plus's) results last.
            $m = Select-String -Path $job.Out -Pattern 'Wins: (\d+), Losses: (\d+), Draws: (\d+)' |
                Select-Object -Last 1
            if (-not $m) {
                if (++$failures -ge 3) {
                    throw "Three iterations in a row gave no result; see $($job.Out)"
                }
                Write-Warning "Iteration $($job.K) gave no result (see $($job.Out)); replaying it"
                $running += Start-Iteration $job.K
                continue
            }
            $failures = 0
            $w = [int]$m.Matches[0].Groups[1].Value
            $l = [int]$m.Matches[0].Groups[2].Value
            $d = [int]$m.Matches[0].Groups[3].Value
            # theta += R_k * c_k * (W - L) * delta, with R_k = a_k / c_k^2.
            foreach ($n in $names) {
                $q = $p[$n]
                $ak = $q.A0 / [Math]::Pow($bigA + $job.K, $alpha)
                $rk = $ak / ($job.Ck[$n] * $job.Ck[$n])
                $theta[$n] = Clamp ($theta[$n] + $rk * $job.Ck[$n] * ($w - $l) * $job.Delta[$n]) $q
            }
            $done++
            $vals = @($names | ForEach-Object { '{0:F3}' -f $theta[$_] })
            "$($job.K),$w,$l,$d," + ($vals -join ',') | Add-Content -Encoding ascii $State
            Remove-Item $job.Out, (Join-Path $tmp "$($job.K).err"), (Join-Path $tmp "$($job.K).json") -ErrorAction SilentlyContinue
            $shown = @($names | ForEach-Object { "$_=$('{0:F1}' -f $theta[$_])" }) -join ' '
            Write-Host ("[{0}/{1}] +{2} -{3} ={4}  {5}" -f $done, $Iterations, $w, $l, $d, $shown)
        }
    }
    $finished = $true
} finally {
    # On Ctrl+C, don't leave fastchess (and its engines) running.
    foreach ($job in $running) {
        if (-not $job.Proc.HasExited) { & taskkill /PID $job.Proc.Id /T /F | Out-Null }
    }
    # Keep the fastchess output of a failed tune for a look.
    if ($finished) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}

Write-Host "Final values (rounded):"
foreach ($n in $names) { Write-Host "  $n=$([Math]::Round($theta[$n]))" }
