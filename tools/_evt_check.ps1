$events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000 } -MaxEvents 40 |
    Where-Object { $_.Message -match 'appVGRPresenterUI' }
if (-not $events) {
    Write-Output "NO appVGRPresenterUI crash events found"
    exit
}
foreach ($e in $events) {
    $lines = $e.Message -split "`n"
    $time  = $e.TimeCreated
    $code  = ($lines | Select-String -Pattern 'Exception code').Line
    $offset = ($lines | Select-String -Pattern 'Faulting module|faulting module').Line
    Write-Output ("{0} | {1} | {2}" -f $time, $code, $offset)
}
