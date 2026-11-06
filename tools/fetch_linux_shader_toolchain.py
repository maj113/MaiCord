#!/usr/bin/env python3
"""Fetches and builds the pinned shader toolchain of the Linux plugin-effect path into a cache directory.

The client's own shaders are hand-written GLSL (src/render/shaders/gles) and need none of this. What needs it is the GLSL
section of a plugin effect (tools/glsl_translate.py, tools/maicord-plugin/mcfx_tool.py effect build --glsl). tools/linux_shader_toolchain.json
pins what that translation is made with:

  * DXC, the release tarball of microsoft/DirectXShaderCompiler (size and SHA-256 checked, extracted once);
  * SPIRV-Cross, built from the pinned tag (the commit is checked) as static libraries, and
    tools/shadertool/mc_shadertool.cpp, the host tool that links them.

Everything lives under $MAICORD_TOOLCHAIN_CACHE (default ~/.cache/maicord-toolchain), outside any build directory,
so a second run does not fetch again. A step that is already done is skipped:
the directories are named by the pin they hold and carry a .ready marker written last. Needs git, cmake, a C++17
compiler (CXX, else c++) and, for the first run, network access. Stdlib only.

A distribution that builds effects offline from its own packages names them instead of the pins, and nothing is fetched or
built but mc_shadertool: MAICORD_DXC is the dxc executable and MAICORD_SPIRV_CROSS the prefix of an installed
SPIRV-Cross (include/spirv_cross/spirv_glsl.hpp and lib/libspirv-cross-glsl.a, lib/libspirv-cross-core.a), both or
neither. The translation then comes from those versions' output.

  fetch_linux_shader_toolchain.py          make sure the toolchain exists and print its paths as JSON
"""
import fcntl
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
PINS = ROOT / 'tools' / 'linux_shader_toolchain.json'
TOOL_SOURCE = ROOT / 'tools' / 'shadertool' / 'mc_shadertool.cpp'


def cache_directory():
    configured = os.environ.get('MAICORD_TOOLCHAIN_CACHE')
    return pathlib.Path(configured) if configured else pathlib.Path.home() / '.cache' / 'maicord-toolchain'


def run(command, **keywords):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **keywords)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        raise SystemExit('error: %s failed (%d)' % (' '.join(str(part) for part in command[:3]), result.returncode))
    return result.stdout


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def fetch_dxc(pins, cache):
    pin = pins['dxc']
    directory = cache / ('dxc-' + pin['sha256'][:12])
    executable = directory / pin['executable']
    if (directory / '.ready').exists() and executable.exists():
        return executable
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    with tempfile.TemporaryDirectory(dir=cache) as scratch:
        archive = pathlib.Path(scratch) / 'dxc.tar.gz'
        print('fetching DXC %s' % pin['version'], file=sys.stderr)
        with urllib.request.urlopen(pin['url'], timeout=120) as response, open(archive, 'wb') as stream:
            shutil.copyfileobj(response, stream)
        if archive.stat().st_size != pin['bytes'] or sha256_of(archive) != pin['sha256']:
            raise SystemExit('error: the DXC archive does not match tools/linux_shader_toolchain.json')
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                target = (directory / member.name).resolve()
                if directory.resolve() not in target.parents and target != directory.resolve():
                    raise SystemExit('error: the DXC archive has a member outside its directory')
            tar.extractall(directory)
    if not executable.exists():
        raise SystemExit('error: the DXC archive has no ' + pin['executable'])
    executable.chmod(0o755)
    (directory / '.ready').write_text(pin['version'])
    return executable


def compiler():
    return os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


def build_spirv_cross(pins, cache):
    pin = pins['spirv_cross']
    directory = cache / ('spirv-cross-' + pin['commit'][:12])
    install = directory / 'install'
    if (directory / '.ready').exists():
        return install
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    source = directory / 'source'
    print('building SPIRV-Cross %s' % pin['tag'], file=sys.stderr)
    run(['git', 'clone', '--quiet', '--depth', '1', '--branch', pin['tag'], pin['repository'], str(source)])
    commit = run(['git', 'rev-parse', 'HEAD'], cwd=source).strip()
    if commit != pin['commit']:
        raise SystemExit('error: tag %s is commit %s, not the pinned %s' % (pin['tag'], commit, pin['commit']))
    generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
    build = directory / 'build'
    configure = ['cmake', '-S', str(source), '-B', str(build), *generator, '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_INSTALL_PREFIX=' + str(install), '-DSPIRV_CROSS_ENABLE_TESTS=OFF', '-DSPIRV_CROSS_CLI=OFF',
                 '-DSPIRV_CROSS_STATIC=ON', '-DSPIRV_CROSS_SHARED=OFF', '-DSPIRV_CROSS_ENABLE_GLSL=ON',
                 '-DSPIRV_CROSS_ENABLE_HLSL=OFF', '-DSPIRV_CROSS_ENABLE_MSL=OFF', '-DSPIRV_CROSS_ENABLE_CPP=OFF',
                 '-DSPIRV_CROSS_ENABLE_REFLECT=OFF', '-DSPIRV_CROSS_ENABLE_C_API=OFF',
                 '-DSPIRV_CROSS_ENABLE_UTIL=OFF', '-DSPIRV_CROSS_SKIP_INSTALL=OFF']
    cxx = compiler()
    if cxx:
        configure.append('-DCMAKE_CXX_COMPILER=' + cxx)
    run(configure)
    run(['cmake', '--build', str(build), '--parallel', str(os.cpu_count() or 4)])
    run(['cmake', '--install', str(build)])
    shutil.rmtree(source, ignore_errors=True)
    shutil.rmtree(build, ignore_errors=True)
    (directory / '.ready').write_text(pin['commit'])
    return install


def build_tool(identity, install, cache):
    cxx = compiler()
    if not cxx:
        raise SystemExit('error: no C++ compiler (set CXX)')
    version = run([cxx, '--version']).splitlines()[0]
    key = hashlib.sha256((TOOL_SOURCE.read_text() + identity + version).encode()).hexdigest()
    directory = cache / ('mc_shadertool-' + key[:12])
    tool = directory / 'mc_shadertool'
    if (directory / '.ready').exists() and tool.exists():
        return tool
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    headers = next((install / 'include').glob('spirv_cross*'))
    libraries = install / 'lib'
    run([cxx, '-std=c++17', '-O2', '-I' + str(headers), str(TOOL_SOURCE), '-L' + str(libraries),
         '-lspirv-cross-glsl', '-lspirv-cross-core', '-o', str(tool)])
    (directory / '.ready').write_text(key)
    return tool


def external_toolchain():
    """(dxc path, SPIRV-Cross prefix) named by MAICORD_DXC and MAICORD_SPIRV_CROSS, or None when neither is set."""
    dxc, prefix = os.environ.get('MAICORD_DXC'), os.environ.get('MAICORD_SPIRV_CROSS')
    if not dxc and not prefix:
        return None
    if not dxc or not prefix:
        raise SystemExit('error: MAICORD_DXC and MAICORD_SPIRV_CROSS are set together or not at all')
    dxc, prefix = pathlib.Path(dxc), pathlib.Path(prefix)
    if not dxc.is_file() or not os.access(dxc, os.X_OK):
        raise SystemExit('error: MAICORD_DXC=%s is not an executable' % dxc)
    if not any((prefix / 'include').glob('spirv_cross*/spirv_glsl.hpp')):
        raise SystemExit('error: MAICORD_SPIRV_CROSS=%s has no include/spirv_cross/spirv_glsl.hpp' % prefix)
    for library in ('glsl', 'core'):
        if not (prefix / 'lib' / ('libspirv-cross-%s.a' % library)).is_file():
            raise SystemExit('error: MAICORD_SPIRV_CROSS=%s has no lib/libspirv-cross-%s.a' % (prefix, library))
    return dxc, prefix


def ensure_toolchain():
    """Returns (dxc path, mc_shadertool path), fetching and building what the cache lacks."""
    pins = json.loads(PINS.read_text())
    cache = cache_directory()
    cache.mkdir(parents=True, exist_ok=True)
    with open(cache / '.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        external = external_toolchain()
        if external:
            dxc, install = external
            tool = build_tool('external ' + str(install), install, cache)
            return dxc, tool
        dxc = fetch_dxc(pins, cache)
        install = build_spirv_cross(pins, cache)
        tool = build_tool(pins['spirv_cross']['commit'], install, cache)
    return dxc, tool


def main():
    dxc, tool = ensure_toolchain()
    print(json.dumps({'dxc': str(dxc), 'mc_shadertool': str(tool)}))


if __name__ == '__main__':
    main()
