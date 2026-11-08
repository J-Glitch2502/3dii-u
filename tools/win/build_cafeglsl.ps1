# Builds CafeGLSL v0.2.0's offline shader compiler as a Windows program, third_party/cafeglsl/glslcompiler.exe. Only needed if that file is ever lost.
# Upstream's own v0.2.0 is Linux only, and its Windows build is a later version with a new Mesa whose vertex shaders draw wrong, so this builds the version the console has been running.
# Its pixel shaders are byte-identical to the Linux build's; a few vertex shaders differ by no more than the Linux build differs from itself between two runs.
# Needs MSYS2 with mingw-w64-x86_64-gcc, flex, bison and ninja (C:\msys64), Windows Python and git. Source and build stay in C:\devkitPro\src, a path with no spaces, which meson, flex and bison need.
# About half an hour at the default 4 jobs, at below-normal priority. Delete C:\devkitPro\src afterwards.
param([int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$src = 'C:\devkitPro\src\CafeGLSL'
$venv = 'C:\devkitPro\src\venv'
$out = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path + '\third_party\cafeglsl'
if (-not (Test-Path -LiteralPath "$src\meson.build")) {
  git -c core.longpaths=true -c core.autocrlf=false clone --quiet --depth 1 --branch v0.2.0 https://github.com/Exzap/CafeGLSL.git $src
  git -C $src -c core.autocrlf=false apply (Join-Path $PSScriptRoot 'cafeglsl_windows.patch')
}
if (-not (Test-Path -LiteralPath "$venv\Scripts\meson.exe")) {
  python -m venv $venv
  & "$venv\Scripts\python.exe" -m pip install --quiet meson mako setuptools packaging
  # meson looks for python3 first, and MSYS2's has no mako
  Copy-Item -LiteralPath "$venv\Scripts\python.exe" -Destination "$venv\Scripts\python3.exe"
}
$env:PATH = "$venv\Scripts;C:\msys64\mingw64\bin;$env:PATH;C:\msys64\usr\bin"
Set-Location -LiteralPath $src
if (-not (Test-Path -LiteralPath "$src\build-win\build.ninja")) {
  meson setup build-win -Db_sanitize=none -Dtools=glsl -Dvulkan-drivers= -Dgallium-drivers=r600 -Db_lundef=false -Dglx=disabled -Degl=disabled -Dplatforms= -Dllvm=disabled -Dzstd=disabled --buildtype=release
}
$p = Start-Process -FilePath 'C:\msys64\mingw64\bin\ninja.exe' -ArgumentList @('-C', 'build-win', '-j', "$Jobs", 'cafecompiler/glslcompiler.elf.exe') -NoNewWindow -PassThru
$p.PriorityClass = 'BelowNormal'
$p.WaitForExit()
if (-not (Test-Path -LiteralPath "$src\build-win\cafecompiler\glslcompiler.elf.exe")) { throw 'the build failed' }
Copy-Item -LiteralPath "$src\build-win\cafecompiler\glslcompiler.elf.exe" -Destination "$out\glslcompiler.exe" -Force
# it needs these two beside it
foreach ($d in 'libwinpthread-1.dll', 'zlib1.dll') { Copy-Item -LiteralPath "C:\msys64\mingw64\bin\$d" -Destination "$out\$d" -Force }
"built $out\glslcompiler.exe"
