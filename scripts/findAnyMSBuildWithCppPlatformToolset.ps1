[CmdletBinding()]
param(
    [Parameter(Mandatory=$False)]
    [string]$explicitlyRequestedVSPath = ""
)

$explicitlyRequestedVSPath = $explicitlyRequestedVSPath -replace "\\$" # Remove potential trailing backslash

$scriptsDir = split-path -parent $MyInvocation.MyCommand.Definition
$VisualStudioInstallationInstances = & $scriptsDir\findVisualStudioInstallationInstances.ps1
Write-Verbose "VS Candidates:`n`r$([system.String]::Join([Environment]::NewLine, $VisualStudioInstallationInstances))"
foreach ($instanceCandidate in $VisualStudioInstallationInstances)
{
    Write-Verbose "Inspecting: $instanceCandidate"
    $split = $instanceCandidate -split "::"
    # $preferenceWeight = $split[0]
    # $releaseType = $split[1]
    $version = $split[2]
    $path = $split[3]

    if ($explicitlyRequestedVSPath -ne "" -and $explicitlyRequestedVSPath -ne $path)
    {
        Write-Verbose "Skipping: $instanceCandidate"
        continue
    }

    $majorVersion = $version.Substring(0,2);
    if ($majorVersion -eq "15")
    {
        $VCFolder= "$path\VC\Tools\MSVC\"
        if (Test-Path $VCFolder)
        {
            Write-Verbose "Picking: $instanceCandidate"
            return "$path\MSBuild\15.0\Bin\MSBuild.exe", "v141"
        }
    }

    if ($majorVersion -eq "14")
    {
        $clExe= "$path\VC\bin\cl.exe"
        if (Test-Path $clExe)
        {
            Write-Verbose "Picking: $instanceCandidate"
            $programFilesPath = & $scriptsDir\getProgramFiles32bit.ps1
            return "$programFilesPath\MSBuild\14.0\Bin\MSBuild.exe", "v140"
        }
    }
}

throw "Could not find MSBuild version with C++ support. VS2015 or VS2017 (with C++) needs to be installed."