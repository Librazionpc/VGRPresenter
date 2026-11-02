Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 6 -ErrorAction SilentlyContinue |
    ForEach-Object {
        $lines = $_.Message -split "`n"
        $head = ($lines | Select-Object -First 6) -join ' | '
        Write-Output ($_.TimeCreated.ToString('HH:mm:ss') + ' :: ' + $head)
    }
