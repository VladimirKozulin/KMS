Write-Host "Compiling MemoryScanner.cs for x64..." -ForegroundColor Cyan
& C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /platform:x64 /nologo /out:MemoryScanner.exe MemoryScanner.cs

if ($LASTEXITCODE -eq 0) {
    Write-Host "Compilation successful! Running MemoryScanner.exe..." -ForegroundColor Green
    & .\MemoryScanner.exe
} else {
    Write-Host "Compilation failed!" -ForegroundColor Red
}
