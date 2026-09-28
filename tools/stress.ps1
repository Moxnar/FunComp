<#
.SYNOPSIS
Stress-tests an engine: many fast games against itself, then a report of
how the games ended and anything that went wrong.

.DESCRIPTION
The checks before a release: no time losses, crashes, disconnects,
illegal moves or stalls under load (24 games at once by default, one per
hardware thread, which makes the timing as hard as it gets on this
machine). Resign adjudication is off, so won games are played out to
mate; the report measures how many plies a side needed to finish once
its score passed +10 pawns, which shows whether a mop-up term is needed.
Draw adjudication stays on (it doesn't hide anything that matters here).

.EXAMPLE
# The two runs on the release list: 1s + 0.01s, and 40 moves in 10 s.
tools\stress.ps1 -Engine engines\best.exe -Book books\8moves_v3.pgn -TC '1+0.01' -Pgn matches\stress-fast.pgn
tools\stress.ps1 -Engine engines\best.exe -Book books\8moves_v3.pgn -TC '40/10' -Pgn matches\stress-40.pgn
#>
param(
    [Parameter(Mandatory)] [string] $Engine,
    [Parameter(Mandatory)] [string] $Book,
    [Parameter(Mandatory)] [string] $Pgn,
    [string] $Fastchess = 'tools\fastchess\fastchess-windows-x86-64\fastchess.exe',
    [string] $TC = '1+0.01',
    [int] $Games = 2000,
    [int] $Hash = 16,
    [int] $Concurrency = 24
)

$ErrorActionPreference = 'Stop'
foreach ($f in @($Engine, $Book, $Fastchess)) {
    if (-not (Test-Path $f)) { throw "Not found: $f" }
}
$enginePath = (Resolve-Path $Engine).Path
$format = if ($Book -match '\.pgn$') { 'pgn' } else { 'epd' }
$log = [IO.Path]::ChangeExtension($Pgn, '.log')

$fcArgs = @(
    '-engine', "cmd=$enginePath", 'name=a',
    '-engine', "cmd=$enginePath", 'name=b',
    '-each', "tc=$TC", "option.Hash=$Hash",
    '-openings', "file=$((Resolve-Path $Book).Path)", "format=$format", 'order=random',
    '-rounds', [Math]::Max(1, [int]($Games / 2)), '-games', 2, '-repeat',
    '-concurrency', $Concurrency,
    '-draw', 'movenumber=40', 'movecount=8', 'score=10',
    '-pgnout', "file=$Pgn",
    '-config', "outname=$([IO.Path]::ChangeExtension($Pgn, '.json'))"
)
Write-Host "Playing $Games games at $TC, $Concurrency at a time..."
$ErrorActionPreference = 'Continue'
& $Fastchess @fcArgs *> $log
$ErrorActionPreference = 'Stop'

# How the games ended, and the failures that matter.
$endings = Select-String -Path $log -Pattern '^Finished game \d+ \([^)]*\): \S+ \{(.*)\}' |
    ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\(\d+ms overrun\)', '(overrun)' }
Write-Host ""
Write-Host "Endings ($($endings.Count) games):"
$endings | Group-Object | Sort-Object Count -Descending |
    ForEach-Object { Write-Host ("  {0,6}  {1}" -f $_.Count, $_.Name) }
$bad = @($endings | Where-Object { $_ -match 'time|disconnect|illegal|stall|crash|timeout' })
$warnings = @(Select-String -Path $log -Pattern '^Warning' | Where-Object { $_.Line -notmatch 'PV continues' })
Write-Host ""
Write-Host "Failures (time losses, disconnects, illegal moves, stalls): $($bad.Count)"
Write-Host "fastchess warnings other than PV notes: $($warnings.Count)"
$warnings | Select-Object -First 10 | ForEach-Object { Write-Host "  $($_.Line)" }

# Conversion: in decisive games that ended in mate, plies from the first
# score of +10 pawns or more (for the winner) to the end.
$plies = @()
$text = Get-Content $Pgn -Raw
foreach ($game in ($text -split '(?=\[Event )')) {
    if ($game -notmatch '\[Result "(1-0|0-1)"\]') { continue }
    if ($game -notmatch '\{[^}]*mates[^}]*\}\s*(1-0|0-1)') { continue }
    $winnerWhite = $Matches[1] -eq '1-0'
    $scores = [regex]::Matches($game, '\{([+-]?[0-9.]+|[+-]?M\d+)/') | ForEach-Object { $_.Groups[1].Value }
    for ($i = 0; $i -lt $scores.Count; $i++) {
        $whiteToMove = ($i % 2) -eq 0  # approximate: book games may start with Black
        $s = $scores[$i]
        $big = if ($s -match 'M') { $s -notmatch '^-' } else { [double]$s -ge 10 }
        $forWinner = ($whiteToMove -eq $winnerWhite)
        if ($big -and $forWinner) { $plies += ($scores.Count - $i); break }
    }
}
if ($plies.Count) {
    $sorted = $plies | Sort-Object
    Write-Host ""
    Write-Host ("Conversion, {0} mates: plies from +10 pawns to mate: median {1}, 90% {2}, max {3}" -f
        $plies.Count, $sorted[[Math]::Floor($sorted.Count / 2)],
        $sorted[[Math]::Min($sorted.Count - 1, [Math]::Floor($sorted.Count * 0.9))], $sorted[-1])
}
