# Write a PicoOS image to a USB stick exactly as it is.
#
# Rufus and the rest of them want to understand what they are given: they
# look for a partition table they recognise, a bootloader they can install,
# a file system they can fill. PicoOS has its own ideas about all three -- it
# boots from a partition table that is written into the image itself -- and a
# tool that tries to improve it generally produces a stick that does not
# start. A stick needs none of that. It needs these bytes copied onto it
# unchanged, which is what this does.
#
# Run it from the .bat next to it, or:
#     powershell -ExecutionPolicy Bypass -File writeusb.ps1 picoos-usb.img
#
# You must be administrator: writing to a whole disk, and not to a file on
# one, is not something Windows lets a normal program do.

param([string]$Image, [switch]$Repair)

$ErrorActionPreference = "Stop"

function Say($s) { Write-Host "  $s" }

Write-Host ""
Write-Host "  PicoOS -- write an image to a usb stick"
Write-Host "  ---------------------------------------"

# --- administrator? -----------------------------------------------------
$who = [Security.Principal.WindowsIdentity]::GetCurrent()
$adm = [Security.Principal.WindowsPrincipal]::new($who)
if (-not $adm.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host ""
    Say "This needs to be run as administrator -- it writes to a whole disk,"
    Say "not to a file on one."
    Say ""
    Say "Right-click writeusb.bat and choose 'Run as administrator'."
    Write-Host ""
    exit 1
}

# --- which stick --------------------------------------------------------
# Only things plugged into the USB bus are offered. Whatever else is true of
# this script, it should not be capable of overwriting the disk Windows is
# running from, and the simplest way to guarantee that is never to offer it.
$disks = Get-Disk | Where-Object { $_.BusType -eq 'USB' -and $_.Size -gt 0 }
if (-not $disks) {
    Say "No usb disk is plugged in. Put the stick in and run this again."
    Write-Host ""
    exit 1
}

Say "Sticks Windows can see:"
Write-Host ""
foreach ($d in $disks) {
    $gb = [math]::Round($d.Size / 1GB, 1)
    $sz = "{0,6} GB" -f $gb
    Say ("   [{0}]  {1}   {2}" -f $d.Number, $sz, $d.FriendlyName)
}
Write-Host ""

$n = Read-Host "  Which number"
if ($n -notmatch '^\d+$') { Say "That is not a number."; exit 1 }
$disk = $disks | Where-Object { [int]$_.Number -eq [int]$n }
if (-not $disk) { Say "There is no usb disk numbered $n."; exit 1 }

# --- give the stick back -------------------------------------------------
# Writing a raw image leaves a stick in a state Windows does not recognise:
# a partition table it did not create, sometimes half a GPT, and no file
# system it can mount. Explorer asks whether you want to format it, and a
# fair few tools -- Rufus among them -- look at the mess and refuse to write
# to it again, with an error that reads like the image is at fault. It is
# not, and it happens with every image because it is the stick.
#
# -Repair wipes all of that away and hands back an ordinary empty stick,
# which is also the state it needs to be in before a raw write.
if ($Repair) {
    Write-Host ""
    Say "This wipes disk $($disk.Number) clean and gives it back as one"
    Say "empty FAT32 partition, the way it was before any of this."
    Say "Everything on it goes."
    $go = Read-Host "  Type YES to go ahead"
    if ($go -ne 'YES') { Say "Nothing was done."; Write-Host ""; exit 1 }

    Say "Cleaning..."
    try {
        Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false
        Initialize-Disk -Number $disk.Number -PartitionStyle MBR
        $p = New-Partition -DiskNumber $disk.Number -UseMaximumSize `
                           -IsActive -AssignDriveLetter
        Format-Volume -DriveLetter $p.DriveLetter -FileSystem FAT32 `
                      -NewFileSystemLabel "PICOOS" -Confirm:$false | Out-Null
    } catch {
        Say "Windows would not do it: $($_.Exception.Message)"
        Say ""
        Say "If it says the disk is write protected, the stick has given up."
        Say "A spare one costs less than an hour of this."
        Write-Host ""
        exit 1
    }
    Write-Host ""
    Say "Done. The stick is an ordinary empty disk again, drive $($p.DriveLetter):"
    Say "It will take an image now -- run this again without -Repair."
    Write-Host ""
    exit 0
}

# --- which image --------------------------------------------------------
if (-not $Image) {
    $Image = Read-Host "  Image file (for example picoos-usb.img)"
}
$Image = $Image.Trim('"')
if (-not (Test-Path $Image)) { Say "Cannot find '$Image'."; exit 1 }

$len = (Get-Item $Image).Length
if ($len % 512 -ne 0) {
    Say "That is not a disk image: its size is not a whole number of sectors."
    exit 1
}
if ($len -gt $disk.Size) {
    Say "The image is bigger than the stick."
    exit 1
}

# --- a disk that will not be written to ---------------------------------
# A disk Windows has decided is read-only fails every single write, whatever
# is being written to it. That is worth saying out loud, because the symptom
# it produces -- "write error", on every image you try -- looks exactly like
# a broken image file. Clearing the flag is one call, and if the call will
# not stick then the stick itself is doing the refusing.
if ($disk.IsReadOnly) {
    Say "Windows has that stick marked read-only. Clearing the flag..."
    Set-Disk -Number $disk.Number -IsReadOnly $false -ErrorAction SilentlyContinue
    $disk = Get-Disk -Number $disk.Number
}
if ($disk.IsReadOnly) {
    Write-Host ""
    Say "It is still read-only, so nothing can be written to it at all."
    Say "In the order I would try them:"
    Say "   1. a little switch on the side of the stick, or of the sd"
    Say "      adapter it is sitting in"
    Say "   2. another port, straight into the machine rather than a hub"
    Say "   3. another stick -- one that has started refusing writes is"
    Say "      one that is dying, and it takes every image down with it"
    Write-Host ""
    exit 1
}

Write-Host ""
Say "About to destroy everything on:"
Say ("     disk {0}   {1}   {2}" -f $disk.Number,
     ("{0} GB" -f [math]::Round($disk.Size / 1GB, 1)), $disk.FriendlyName)
Say "and replace it with:"
Say ("     {0}   {1} MB" -f (Split-Path $Image -Leaf),
     ("{0}" -f [math]::Round($len / 1MB, 1)))
Write-Host ""
Say "Everything on that stick goes. There is no undo."
$ok = Read-Host "  Type YES to go ahead"
if ($ok -ne 'YES') { Say "Nothing was written."; Write-Host ""; exit 1 }

# --- let go of the stick ------------------------------------------------
# Windows will not hand out a whole disk for writing while it has volumes
# mounted on it, so the partitions go first. The image carries its own
# partition table, so nothing is lost by clearing the old one.
Write-Host ""
Say "Clearing the stick..."
Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false `
           -ErrorAction SilentlyContinue
Set-Disk -Number $disk.Number -IsOffline $true -ErrorAction SilentlyContinue

# --- write it -----------------------------------------------------------
$path = "\\.\PhysicalDrive" + $disk.Number
Say "Writing $Image -> $path"

$src = $null
$dst = $null
try {
    $src = [System.IO.File]::Open($Image, 'Open', 'Read', 'Read')
    $dst = [System.IO.File]::Open($path, 'Open', 'Write', 'None')

    $buf = New-Object byte[] (1MB)
    $done = 0L
    while (($got = $src.Read($buf, 0, $buf.Length)) -gt 0) {
        $dst.Write($buf, 0, $got)
        $done += $got
        $pct = [int]($done * 100 / $len)
        Write-Progress -Activity "Writing PicoOS" -Status "$pct %" `
                       -PercentComplete $pct
    }
    $dst.Flush()
} catch {
    Write-Host ""
    if ($done -gt 0) {
        Say "The write got $done bytes in and then stopped:"
        Say "     $($_.Exception.Message)"
        Say ""
        Say "A write that starts and then fails part of the way through is a"
        Say "stick that cannot hold what is being put on it. This happens"
        Say "with every image you try because it is the stick, not the"
        Say "image. Try a different one."
    } else {
        Say "Windows would not let me start writing:"
        Say "     $($_.Exception.Message)"
        Say ""
        Say "If it says the disk is in use, take the stick out, put it back,"
        Say "and run this again without opening it in Explorer."
    }
    exit 1
} finally {
    if ($src) { $src.Close() }
    if ($dst) { $dst.Close() }
}

Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction SilentlyContinue

# --- did it land? -------------------------------------------------------
# Read the first sector back and look for the signature every bootable disk
# ends with. It is a small check, but it catches the one failure that looks
# like success: a write that went nowhere.
$f = [System.IO.File]::Open($path, 'Open', 'Read', 'Read')
$sector = New-Object byte[] 512
$f.Read($sector, 0, 512) | Out-Null
$f.Close()

if ($sector[510] -eq 0x55 -and $sector[511] -eq 0xAA) {
    Write-Host ""
    Say "Done. $done bytes written, and the disk signature is there."
    Say "Boot the machine from it."
} else {
    Write-Host ""
    Say "$done bytes went out, but the disk does not look bootable now."
    Say "Try the stick in another port, or another stick."
}
Write-Host ""
