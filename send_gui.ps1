$client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4444)
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.Write("gui`r`n")
$writer.Flush()
Start-Sleep -Seconds 2
$writer.Close()
$client.Close()
