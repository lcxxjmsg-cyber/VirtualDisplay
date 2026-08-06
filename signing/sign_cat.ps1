$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $RepoRoot "build"
$InfFile = Join-Path $RepoRoot "driver\VirtualDisplay.inf"
$DllFile = Join-Path $BuildDir "bin\VirtualDisplayDriver.dll"
$WdfDllFile = Join-Path $ScriptDir "WdfCoInstaller01011.dll"
$WudfRdInfFile = Join-Path $env:SystemRoot "INF\wudfrd.inf"
$CatFile = Join-Path $BuildDir "bin\VirtualDisplay.cat"
$CerFile = Join-Path $ScriptDir "VirtualDisplayTestSigning.cer"
$RootCerFile = Join-Path $ScriptDir "VirtualDisplayTestRoot.cer"
$SigningDir = Join-Path $BuildDir "signing"
$PfxFile = Join-Path $SigningDir "VirtualDisplayTestSigning.pfx"
$RootPfxFile = Join-Path $SigningDir "VirtualDisplayTestRoot.pfx"
$PfxPassword = "VirtualDisplayTestSigning"
$ChainVersionFile = Join-Path $SigningDir "root-leaf-v1.txt"

$MakeCat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\makecat.exe"
$SignTool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
$OpenSsl = "C:\msys64\ucrt64\bin\openssl.exe"

if (-not (Test-Path $InfFile)) {
    Write-Error "INF not found: $InfFile"
}
if (-not (Test-Path $DllFile)) {
    Write-Error "Driver DLL not found: $DllFile"
}

Write-Host "=== Generating and signing VirtualDisplay catalog ==="
Write-Host ""

# Create temp dir
$TmpDir = Join-Path $BuildDir "cat_tmp"
if (Test-Path $TmpDir) { Remove-Item -Recurse -Force $TmpDir }
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

# Copy files
Copy-Item $InfFile $TmpDir
Copy-Item $DllFile $TmpDir
if (Test-Path $WdfDllFile) {
    Copy-Item $WdfDllFile $TmpDir
} else {
    Write-Warning "WdfCoInstaller01011.dll not found; catalog will omit it"
}
if (Test-Path $WudfRdInfFile) {
    Copy-Item $WudfRdInfFile $TmpDir
} else {
    Write-Warning "wudfrd.inf not found; catalog will omit it"
}

# Create CDF file with CRLF line endings (makecat requires them)
$cdfFiles = @(
    "File1=VirtualDisplay.inf",
    "File2=VirtualDisplayDriver.dll"
)
if (Test-Path (Join-Path $TmpDir "WdfCoInstaller01011.dll")) {
    $cdfFiles += "File3=WdfCoInstaller01011.dll"
}
if (Test-Path (Join-Path $TmpDir "wudfrd.inf")) {
    $cdfFiles += "File4=wudfrd.inf"
}

$cdfContent = "[CatalogHeader]$([char]13)$([char]10)" +
"Name=VirtualDisplay.cat$([char]13)$([char]10)" +
"ResultDir=$TmpDir$([char]13)$([char]10)" +
"CatalogVersion=2$([char]13)$([char]10)" +
"HashAlgorithms=SHA256$([char]13)$([char]10)" +
"PageHashes=FALSE$([char]13)$([char]10)" +
"$([char]13)$([char]10)" +
"[CatalogFiles]$([char]13)$([char]10)"
foreach ($entry in $cdfFiles) {
    $cdfContent += "<HASH>$entry$([char]13)$([char]10)"
}

$cdfPath = Join-Path $TmpDir "VirtualDisplay.cdf"
Set-Content -Path $cdfPath -Value $cdfContent -Encoding Ascii -NoNewline

Write-Host "Creating catalog..."
Push-Location $TmpDir
& $MakeCat /v VirtualDisplay.cdf 2>&1
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "makecat failed"
}
Pop-Location

# Copy catalog to output dir
$tmpCat = Join-Path $TmpDir "VirtualDisplay.cat"
if (Test-Path $tmpCat) {
    Copy-Item $tmpCat $CatFile -Force
}

if (-not (Test-Path $CatFile)) {
    Write-Error "Catalog file was not created"
}

# Generate an isolated test-signing chain when one does not exist. Windows
# driver staging rejects a self-signed catalog signer on some Win10 builds, so
# the root CA and the code-signing leaf are kept separate.
if ((-not (Test-Path $PfxFile)) -or (-not (Test-Path $RootPfxFile)) -or (-not (Test-Path $ChainVersionFile))) {
    if (-not (Test-Path $OpenSsl)) {
        Write-Error "OpenSSL not found at $OpenSsl"
    }

    New-Item -ItemType Directory -Path $SigningDir -Force | Out-Null
    $rootKeyFile = Join-Path $SigningDir "VirtualDisplayTestRoot.key.pem"
    $rootPemFile = Join-Path $SigningDir "VirtualDisplayTestRoot.cert.pem"
    $signingKeyFile = Join-Path $SigningDir "VirtualDisplayTestSigning.key.pem"
    $signingCsrFile = Join-Path $SigningDir "VirtualDisplayTestSigning.csr.pem"
    $signingPemFile = Join-Path $SigningDir "VirtualDisplayTestSigning.cert.pem"
    $signingExtFile = Join-Path $SigningDir "VirtualDisplayTestSigning.ext"

    Write-Host "Generating test-signing root certificate..."
    & $OpenSsl req -x509 -newkey rsa:2048 -sha256 -nodes `
        -keyout $rootKeyFile `
        -out $rootPemFile `
        -days 3650 `
        -subj "/CN=VirtualDisplay Test Root CA" `
        -addext "basicConstraints=critical,CA:TRUE,pathlen:1" `
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "OpenSSL root certificate generation failed"
    }

    Write-Host "Generating test-signing leaf certificate..."
    & $OpenSsl req -newkey rsa:2048 -sha256 -nodes `
        -keyout $signingKeyFile `
        -out $signingCsrFile `
        -subj "/CN=VirtualDisplay Test Signing" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "OpenSSL signing CSR generation failed"
    }

    $signingExtContent = "basicConstraints=critical,CA:FALSE$([char]10)" +
        "keyUsage=critical,digitalSignature$([char]10)" +
        "extendedKeyUsage=codeSigning$([char]10)" +
        "subjectKeyIdentifier=hash$([char]10)" +
        "authorityKeyIdentifier=keyid,issuer$([char]10)"
    Set-Content -Path $signingExtFile -Value $signingExtContent -Encoding Ascii -NoNewline

    & $OpenSsl x509 -req `
        -in $signingCsrFile `
        -CA $rootPemFile `
        -CAkey $rootKeyFile `
        -CAcreateserial `
        -out $signingPemFile `
        -days 3650 `
        -sha256 `
        -extfile $signingExtFile 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "OpenSSL signing certificate generation failed"
    }

    & $OpenSsl pkcs12 -export `
        -out $PfxFile `
        -inkey $signingKeyFile `
        -in $signingPemFile `
        -certfile $rootPemFile `
        -passout "pass:$PfxPassword" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "OpenSSL signing PFX generation failed"
    }

    & $OpenSsl pkcs12 -export `
        -out $RootPfxFile `
        -inkey $rootKeyFile `
        -in $rootPemFile `
        -passout "pass:$PfxPassword" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "OpenSSL root PFX generation failed"
    }

    Set-Content -Path $ChainVersionFile -Value "root-leaf-v1" -Encoding Ascii -NoNewline
    Remove-Item $rootKeyFile, $rootPemFile, $signingKeyFile, $signingCsrFile, $signingPemFile, $signingExtFile -Force
}

$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2
$certificate.Import($PfxFile, $PfxPassword, [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::DefaultKeySet)
$certBytes = $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
[System.IO.File]::WriteAllBytes($CerFile, $certBytes)

$rootCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2
$rootCertificate.Import($RootPfxFile, $PfxPassword, [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::DefaultKeySet)
$rootCertBytes = $rootCertificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
[System.IO.File]::WriteAllBytes($RootCerFile, $rootCertBytes)

# Sign the catalog
Write-Host "Signing catalog..."
& $SignTool sign /v /f $PfxFile /p $PfxPassword /ac $RootCerFile /fd SHA256 $CatFile 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "signtool signing failed"
}

# Verify. A freshly generated test certificate may not be trusted on the build
# host yet, so a trust-chain failure here is reported as a warning only.
$verifyOutput = & cmd.exe /c "`"$SignTool`" verify /v /pa `"$CatFile`" 2>&1"
$verifyExitCode = $LASTEXITCODE
$verifyOutput | ForEach-Object { Write-Host $_ }
if ($verifyExitCode -ne 0) {
    Write-Warning "Catalog signature was created, but local trust verification failed. Install VirtualDisplayTestRoot.cer into Root and VirtualDisplayTestSigning.cer into TrustedPublisher before installing the driver package."
}

Write-Host ""
Write-Host "Catalog signed successfully: $CatFile"
Write-Host "Signing certificate exported: $CerFile"
Write-Host "Root certificate exported: $RootCerFile"
Write-Host ""
Write-Host "Deployment package:"
Write-Host "  $DllFile"
Write-Host "  $CatFile"
Write-Host "  $InfFile"
Write-Host "  $CerFile"
Write-Host "  $RootCerFile"
