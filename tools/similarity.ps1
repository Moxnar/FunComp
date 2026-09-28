<#
.SYNOPSIS
Move-similarity test: how often engines pick the same best move.

.DESCRIPTION
Every engine searches the same sample of positions for a fixed time, and
the script reports, for each pair of engines, the share of positions on
which they chose the same move. This is the community's usual clone
check: unrelated engines of similar strength agree on a certain share of
moves, and an engine derived from another agrees with it far more often.
The baseline is the agreement among the unrelated engines in the run.

Each engine runs in its own process, all at once (one core each, Hash as
given, default threads). Positions are sampled from an EPD or FEN file
(one position per line) with a fixed seed, and written out with the
results, so a run can be repeated exactly.

.EXAMPLE
tools\similarity.ps1 -Engines engines\best.exe,engines\stockfish-19.exe,engines\stash-33.0.exe,engines\reckless-0.5.0.exe -Positions books\UHO_Lichess_4852_v1.epd
#>
param(
    [Parameter(Mandatory)] [string[]] $Engines,
    [Parameter(Mandatory)] [string] $Positions,
    [int] $Count = 2000,
    # Search time per position, ms.
    [int] $MoveTime = 100,
    [int] $Hash = 16,
    [int] $Seed = 1,
    # Output prefix: <Out>-positions.txt, <Out>-<engine>.txt, <Out>.txt.
    [string] $Out = 'matches\similarity'
)

$ErrorActionPreference = 'Stop'

foreach ($exe in $Engines + $Positions) {
    if (-not (Test-Path $exe)) { throw "Not found: $exe" }
}

# Sample the positions. EPD lines carry 4 FEN fields and then operations;
# FEN lines carry 6 fields. Either way the first 4 fields are kept, with
# the move counters reset.
# Books run to millions of lines, so the sample is drawn in one streaming
# pass (reservoir sampling), not by sorting the whole file.
$rng = New-Object System.Random $Seed
$reservoir = New-Object 'System.Collections.Generic.List[string]'
$seen = 0
foreach ($line in [IO.File]::ReadLines((Resolve-Path $Positions).Path)) {
    if (-not $line.Trim()) { continue }
    if ($reservoir.Count -lt $Count) { $reservoir.Add($line) }
    else {
        $j = $rng.Next($seen + 1)
        if ($j -lt $Count) { $reservoir[$j] = $line }
    }
    ++$seen
}
$sample = foreach ($line in $reservoir) {
    $f = $line.Trim() -split '\s+'
    ($f[0..3] -join ' ') + ' 0 1'
}
$sample | Set-Content -Encoding ascii "$Out-positions.txt"
Write-Host "$($sample.Count) positions from $Positions, $MoveTime ms each"

# One engine over every position: the best move for each, or '-' if the
# engine gave none.
$run = {
    param($exe, $fens, $moveTime, $hash)
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo.FileName = $exe
    $p.StartInfo.WorkingDirectory = Split-Path $exe
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardInput = $true
    $p.StartInfo.RedirectStandardOutput = $true
    $p.StartInfo.CreateNoWindow = $true
    [void] $p.Start()
    # Commands go out as plain ASCII bytes: Windows PowerShell's stdin
    # writer may lead with a UTF-8 byte-order mark, which some engines
    # take as part of the first command.
    $stdin = $p.StandardInput.BaseStream
    $in = New-Object PSObject
    $in | Add-Member ScriptMethod WriteLine {
        param($s)
        $b = [Text.Encoding]::ASCII.GetBytes($s + "`n")
        $stdin.Write($b, 0, $b.Length)
        $stdin.Flush()
    }
    function WaitFor([string] $prefix) {
        while ($true) {
            $line = $p.StandardOutput.ReadLine()
            if ($null -eq $line) { return $null }
            if ($line.StartsWith($prefix)) { return $line }
        }
    }
    $in.WriteLine('uci'); [void] (WaitFor 'uciok')
    $in.WriteLine("setoption name Hash value $hash")
    $in.WriteLine('isready'); [void] (WaitFor 'readyok')
    $moves = foreach ($fen in $fens) {
        $in.WriteLine("position fen $fen")
        $in.WriteLine("go movetime $moveTime")
        $line = WaitFor 'bestmove'
        if ($line) { ($line -split '\s+')[1] } else { '-' }
    }
    $in.WriteLine('quit')
    if (-not $p.WaitForExit(5000)) { $p.Kill() }
    $moves
}

$names = $Engines | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }
$jobs = for ($i = 0; $i -lt $Engines.Count; ++$i) {
    Start-Job -Name $names[$i] -ScriptBlock $run `
        -ArgumentList (Resolve-Path $Engines[$i]).Path, $sample, $MoveTime, $Hash
}
$minutes = [Math]::Ceiling($sample.Count * ($MoveTime + 5) / 60000.0)
Write-Host "Running $($Engines.Count) engines in parallel, about $minutes min"
$results = @{}
foreach ($j in $jobs) {
    $moves = @(Receive-Job $j -Wait)
    Remove-Job $j
    $results[$j.Name] = $moves
    $moves | Set-Content -Encoding ascii "$Out-$($j.Name).txt"
}

# Agreement matrix: percent of positions where both engines answered and
# chose the same move.
$width = [Math]::Max(8, ($names | Measure-Object -Maximum Length).Maximum + 1)
$report = @()
$report += ('{0,-' + $width + '}') -f '' + (($names | ForEach-Object { ('{0,' + $width + '}') -f $_ }) -join '')
foreach ($a in $names) {
    $row = ('{0,-' + $width + '}') -f $a
    foreach ($b in $names) {
        if ($a -eq $b) { $row += ('{0,' + $width + '}') -f '-'; continue }
        $same = 0; $both = 0
        for ($k = 0; $k -lt $sample.Count; ++$k) {
            $x = $results[$a][$k]; $y = $results[$b][$k]
            if ($x -ne '-' -and $y -ne '-') {
                ++$both
                if ($x -eq $y) { ++$same }
            }
        }
        $pct = if ($both) { 100.0 * $same / $both } else { 0 }
        $row += ('{0,' + $width + ':F1}') -f $pct
    }
    $report += $row
}
$report += ''
$report += "Same best move, % of $($sample.Count) positions at $MoveTime ms (seed $Seed)."
$report | Set-Content -Encoding ascii "$Out.txt"
$report | Write-Host
