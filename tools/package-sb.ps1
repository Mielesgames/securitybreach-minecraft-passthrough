# Builds both halves of FNAFSBCraft and packs a release into dist\:
#   FNAFSBCraft-<version>.zip         unpacked into Security Breach's folder (Steam\steamapps\common\Quarters):
#                                     UE4SS (this project's build, with UE4SS 3.0.1's loader dwmapi.dll), the
#                                     mod, and FNAFSBCraft-Minecraft.zip, the Minecraft it starts
#   FNAFSBCraft-<version>-pdb.zip     the mod's and UE4SS's debug symbols, for reading crash dumps
#   skycraft-fabric-<version>.jar     the Minecraft mod on its own (for your own launcher)
#
# FNAFSBCraft-Minecraft.zip holds a portable Prism Launcher with a ready "FNAFSBCraft" instance (Minecraft
# 26.3, Fabric, Fabric API, the mod; -Dskycraft.passthrough=true). The mod unpacks it to
# %LOCALAPPDATA%\FNAFSBCraft and starts it; Prism asks the player to sign in once, then downloads
# Minecraft and Java itself.
#
#   powershell -ExecutionPolicy Bypass -File tools\package-sb.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$root\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# Pinned downloads (checked against these hashes).
$prismVersion = "11.1.1"
$prismZip = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256 = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
$prismLicenseUrl = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
$fabricApiJar = "fabric-api-0.161.0+26.3.jar"
$fabricApiUrl = "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar"
$fabricApiSha512 = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"
$ue4ssZip = "UE4SS_v3.0.1.zip"
$ue4ssUrl = "https://github.com/UE4SS-RE/RE-UE4SS/releases/download/v3.0.1/$ue4ssZip"
$ue4ssSha256 = "4b47d4bceddd2f561a4e395bfa00924ccfc945af576a2d0c613e6537846c57ec"

function Get-Pinned([string]$url, [string]$path, [string]$algorithm, [string]$hash) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    }
    if ($hash -and (Get-FileHash $path -Algorithm $algorithm).Hash -ne $hash.ToUpper()) {
        Remove-Item $path
        throw "$path doesn't match its pinned $algorithm hash"
    }
}

# Zip entries named explicitly with forward slashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$path, [System.Collections.IDictionary]$entries) {
    $zip = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entries[$name], $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}
function New-ZipFromFolder([string]$path, [string]$folder) {
    $entries = [ordered]@{}
    $base = (Resolve-Path $folder).Path.TrimEnd('\') + '\'
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entries[$_.FullName.Substring($base.Length).Replace('\', '/')] = $_.FullName
    }
    New-Zip $path $entries
}

if (-not $NoBuild) {
    & $cmake --build "$root\build" --config Game__Shipping__Win64 --target SecurityBreachMinecraftPassthrough
    if ($LASTEXITCODE) { throw "the Security Breach mod didn't build" }
    Push-Location "$root\fabric"
    try {
        .\gradlew.bat build --no-configuration-cache
        if ($LASTEXITCODE) { throw "the Fabric mod didn't build" }
    } finally { Pop-Location }
}

$dll = "$root\build\sb\Game__Shipping__Win64\SecurityBreachMinecraftPassthrough.dll"
$pdb = "$root\build\sb\Game__Shipping__Win64\SecurityBreachMinecraftPassthrough.pdb"
$ue4ssDll = "$root\build\Game__Shipping__Win64\bin\UE4SS.dll"
$ue4ssPdb = "$root\build\Game__Shipping__Win64\bin\UE4SS.pdb"
$jar = "$root\fabric\build\libs\skycraft-$version.jar"
foreach ($f in @($dll, $pdb, $ue4ssDll, $jar)) {
    if (-not (Test-Path $f)) { throw "missing $f (build first, or drop -NoBuild)" }
}
$cache = "$root\.tools\prism"
Get-Pinned $prismUrl "$cache\$prismZip" SHA256 $prismSha256
Get-Pinned $fabricApiUrl "$cache\$fabricApiJar" SHA512 $fabricApiSha512
Get-Pinned $prismLicenseUrl "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "" ""
Get-Pinned $ue4ssUrl "$cache\$ue4ssZip" SHA256 $ue4ssSha256

$dist = "$root\dist"
New-Item -ItemType Directory $dist -Force | Out-Null
Get-ChildItem $dist | Remove-Item -Recurse -Force

# The bundled Minecraft: Prism (portable), the FNAFSBCraft instance, its mods, Prism's default settings.
$bundle = "$dist\bundle"
Copy-Item -Recurse "$root\tools\fnafsbcraft-bundle" $bundle
Expand-Archive "$cache\$prismZip" "$bundle\Prism" -Force
Copy-Item "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "$bundle\Prism\LICENSE-PrismLauncher.txt"
(Get-Content "$bundle\Prism\THIRD-PARTY.txt" -Raw).Replace("{PRISM_VERSION}", $prismVersion) | Set-Content "$bundle\Prism\THIRD-PARTY.txt" -NoNewline
$mods = "$bundle\Prism\instances\FNAFSBCraft\.minecraft\mods"
New-Item -ItemType Directory $mods -Force | Out-Null
Copy-Item "$cache\$fabricApiJar" $mods
Copy-Item $jar "$mods\skycraft-$version.jar"
Set-Content "$bundle\bundle-version.txt" "FNAFSBCraft $version, Prism Launcher $prismVersion, $fabricApiJar" -NoNewline
New-ZipFromFolder "$dist\FNAFSBCraft-Minecraft.zip" $bundle
Remove-Item -Recurse -Force $bundle

# UE4SS's loader (dwmapi.dll) from the official 3.0.1 release; UE4SS itself is this project's build.
$ue4ssDir = "$dist\ue4ss"
Expand-Archive "$cache\$ue4ssZip" $ue4ssDir -Force

$win64 = "fnaf9/Binaries/Win64"
$modDir = "$win64/Mods/SecurityBreachMinecraftPassthrough"
New-Zip "$dist\FNAFSBCraft-$version.zip" ([ordered]@{
    "$win64/dwmapi.dll" = "$ue4ssDir\dwmapi.dll"
    "$win64/UE4SS.dll" = $ue4ssDll
    "$win64/UE4SS-settings.ini" = "$root\tools\fnafsbcraft-release\UE4SS-settings.ini"
    "$modDir/dlls/main.dll" = $dll
    "$modDir/enabled.txt" = "$root\tools\fnafsbcraft-release\enabled.txt"
    "$modDir/FNAFSBCraft.ini" = "$root\tools\fnafsbcraft-release\FNAFSBCraft.ini"
    "$modDir/FNAFSBCraft-Minecraft.zip" = "$dist\FNAFSBCraft-Minecraft.zip"
    "$modDir/LICENSE.txt" = "$root\LICENSE"
    "$modDir/THIRD-PARTY-NOTICES.md" = "$root\THIRD-PARTY-NOTICES.md"
})
$symbols = [ordered]@{ "SecurityBreachMinecraftPassthrough.pdb" = $pdb }
if (Test-Path $ue4ssPdb) { $symbols["UE4SS.pdb"] = $ue4ssPdb }
New-Zip "$dist\FNAFSBCraft-$version-pdb.zip" $symbols
Copy-Item $jar "$dist\skycraft-fabric-$version.jar"
Remove-Item "$dist\FNAFSBCraft-Minecraft.zip"
Remove-Item -Recurse -Force $ue4ssDir

Get-ChildItem $dist | ForEach-Object { "{0,-44} {1,14:N0} bytes" -f $_.Name, $_.Length }
