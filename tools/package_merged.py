"""Package the verified local host; never writes to the upstream repository."""
from pathlib import Path
import argparse
import difflib
import hashlib
import json
import re
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build', required=True)
parser.add_argument('--release', required=True)
parser.add_argument('--reference', required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
build, release, reference = map(Path, (args.build, args.release, args.reference))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


tests = {}
for name in ('protocol', 'device', 'media', 'app', 'ui', 'analysis', 'plintegration'):
    log = build / f'test_{name}-results.txt'
    text = log.read_text(encoding='utf-8')
    match = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', text)
    if not match or int(match[2]) or int(match[3]) or 'FAIL!' in text:
        raise RuntimeError(f'Not a clean regression: {log}')
    tests[name] = {'passed_including_lifecycle': int(match[1]), 'failed': 0, 'skipped': 0}
if '100% tests passed' not in (build / 'ctest.log').read_text(encoding='utf-8'):
    raise RuntimeError('CTest is not fully passing')
if 'OK' not in (build / 'python-tests.txt').read_text(encoding='utf-8'):
    raise RuntimeError('Python tests did not pass')
if 'self-test passed' not in (build / 'mock-selftest.txt').read_text(encoding='utf-8'):
    raise RuntimeError('Mock self-test did not pass')
if 'exit_code=0' not in (build / 'deployment-smoke.txt').read_text(encoding='utf-8'):
    raise RuntimeError('Standalone deployment smoke did not pass')

exe = release / 'qt/bin/MicArray.exe'
plugin = release / 'qt/plugins/platforms/qwindows.dll'
if not exe.is_file() or not plugin.is_file():
    raise RuntimeError('Deployment is incomplete')
if sha(exe) not in (build / 'deployment-smoke.txt').read_text(encoding='utf-8').lower():
    raise RuntimeError('Deployment smoke did not test this executable hash')
for path in list((source / 'src').rglob('*')) + [source / 'CMakeLists.txt']:
    if path.is_file() and path.suffix in ('.h', '.cpp', '.txt'):
        text = path.read_text(encoding='utf-8')
        if re.search(r'voiceprint|sherpa|speakerembedder|src/voice', text, re.I):
            raise RuntimeError(f'Forbidden voice dependency: {path}')
if subprocess.check_output(['git', '-C', str(reference), 'status', '--porcelain']).strip():
    raise RuntimeError('Reference checkout was modified')
commit = subprocess.check_output(['git', '-C', str(reference), 'rev-parse', 'HEAD'], text=True).strip()

# Localization implementation remains upstream-identical, apart from extra transport types.
algorithm_files = ['arraygeometry', 'gccphatlocator', 'robustsrpphatscorer',
                   'neuralsrppeakrefiner', 'neuralmodeldata', 'radix2fft', 'livelocalizationworker']
preserved_algorithms = []
for stem in algorithm_files:
    for extension in ('h', 'cpp'):
        relative = Path('src/analysis') / f'{stem}.{extension}'
        if (reference / relative).is_file():
            if sha(reference / relative) != sha(source / relative):
                raise RuntimeError(f'Upstream algorithm changed: {relative}')
            preserved_algorithms.append(relative.as_posix())

def ignore_generated(directory, names):
    return [name for name in names
            if name in {'.git', '__pycache__', 'dist', 'ui-test-output.txt'}
            or (name.startswith('build') and (Path(directory) / name).is_dir())]


shutil.copytree(source, release / 'source', dirs_exist_ok=True, ignore=ignore_generated)
if not (release / 'source/scripts/build-merged.ps1').is_file():
    raise RuntimeError('Complete source package must include its build script')
reports = release / 'reports'
reports.mkdir(exist_ok=True)
for name in ('ctest.log', 'build-release.log', 'build-release-final.log', 'python-tests.txt', 'mock-selftest.txt',
             'deployment-smoke.txt', 'deployment-smoke.stderr.txt', 'analysis_parallel_budget_failure.txt',
             'analysis_pre_lto_budget_failure.txt', 'upstream_cpu_budget.txt', 'deployment-offscreen-attempt.txt'):
    if (build / name).is_file():
        shutil.copy2(build / name, reports / name)
for log in build.glob('test_*-results.txt'):
    shutil.copy2(log, reports / log.name)
shutil.copytree(build / 'qa', release / 'screenshots', dirs_exist_ok=True)

diff = []
for path in sorted((source / 'src').rglob('*')):
    if path.is_file() and path.suffix in ('.cpp', '.h'):
        relative = path.relative_to(source)
        original = reference / relative
        before = original.read_text(encoding='utf-8').splitlines(True) if original.is_file() else []
        after = path.read_text(encoding='utf-8').splitlines(True)
        diff.extend(difflib.unified_diff(before, after,
                    fromfile='upstream/' + relative.as_posix(), tofile='merged/' + relative.as_posix()))
(reports / 'upstream-source-diff.patch').write_text(''.join(diff), encoding='utf-8')

validation = {
    'status': 'LOCAL_BUILD_AND_MOCK_VALIDATED_HARDWARE_TEST_PENDING',
    'primary_upstream': 'https://github.com/honokawings/MicArray',
    'upstream_commit': commit,
    'primary_algorithm_files_byte_identical': preserved_algorithms,
    'local_auxiliary': 'MicArray-PL-GitHub-Robust-20261014/source/qt',
    'voiceprint_modules': 0, 'speaker_identity_models': 0,
    'default_localization_source': 'pl', 'pl_heatmap_uploaded': False,
    'pl_display_modes': ['marker', 'fusion'],
    'fusion_heatmap_source': 'host SRP spatial response from unchanged raw48 PCM24',
    'fusion_marker_source': 'PL LOC3 physical angles only',
    'fusion_video_calibration': 'shared projection and unprojection',
    'host_peak_cannot_replace_pl_marker': True,
    'heatmap_expiry': 'max(1000ms, 3 * software updateIntervalMs); independent of PL marker expiry',
    'raw_channels': 48, 'sample_rate_hz': 48000,
    'raw_pcm_order': 'CH0..CH47 unchanged; low PCM24 restored before gain',
    'raw_recording_default': True, 'ps_modified': False, 'bit_modified': False,
    'qt_suites': tests, 'python_tests': 45, 'mock_server_self_test': 'passed',
    'mock_tcp_ui_and_raw48_wav': 'passed in fusion mode; raw software-analysis input and 48ch WAV checked byte for byte',
    'independent_deployment_smoke': 'passed with Qt toolkit removed from PATH',
    'deployment_smoke_platform': 'native qwindows, hidden window',
    'real_board_connected': False,
    'exe_sha256': sha(exe),
    'limitations': ['Mock and software tests are not live board acceptance.',
                    'Fusion heatmap is calculated on the host, not uploaded by the FPGA.',
                    'The PL marker and software response have independent update times and may disagree.',
                    'Current paired FPGA bit publishes one trusted main source.',
                    'Host display tracking cannot correct competing acoustic peaks.',
                    'The CPU-budget suite runs serially; update rate depends on CPU load. Prior non-LTO budget misses are retained in reports.'],
}
(release / 'validation.json').write_text(json.dumps(validation, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
(release / 'README.md').write_text('''# MicArray PL 定位与软件热力图融合版 20261014

双击 `qt/bin/MicArray.exe` 或本目录 `启动上位机.cmd`。

以 GitHub master 上位机为主，保留其定位算法、音频分析、热力图、录音、软件降噪、校准、主题及诊断。合入原程序的 LOC3 接收、物理角度投影、显示跟踪与过期清除；声纹、说话人身份识别和自动人声模块及模型依赖均未进入本版本。

默认连接参数为 192.168.2.10:9968。进入“声源定位”，定位来源保持“FPGA 定位（PL LOC3）”，将显示方式选为“上位机热力图 + PL 定位”。软件依据原始 48 通道 PCM 计算真实热力图，叠加 PL 定位点，两层使用同一视频标定。画面明确标注“热力图：上位机参考 / 定位点：PL”。软件定位峰不会覆盖 PL 标记，PL 校准仍使用 PL 独立定位帧。

本版保留默认“纯标记”，不启动软件参考计算。融合选项及上位机参考模式的显示方式独立保存。透明度和网格等设置仅改变软件热力图，不更改 PL 角度。热力图不是把 PL 点画成彩色光斑，不强行将软件峰移动到 PL 坐标。两层更新节奏不同，混响或竞争峰时可能不同，不能由本次显示功能宣称定位精度或漂移已有改善。

录音默认勾选“录制原始 PCM”。性能测试时按需要全选 48 路；旁带剥离后，通道顺序和原始低 24 位 PCM 数值保持不变。取消勾选可录制增益处理后的音频。

无需更换 bit 或 PS 程序。新移植 PL 目前只上传一个可信主声源。无声源或 PL 过期清除标记；软件热力图独立过期、禁用及断线清除。离开定位页或使用纯标记时停止软件参考计算。

`source/` 为完整可编译源码，`reports/` 为回归与部署记录，`screenshots/` 为模拟输入界面核验，其中融合热力图来自合成的同相宽带音频，画面为测试背景。具体参数和证据见 `validation.json`。通过本地回归和模拟 TCP 全链路测试，尚未连接实物联调。原合并版、GitHub 参考副本及旧程序保留，未上传远端。
''', encoding='utf-8')
(release / '启动上位机.cmd').write_bytes('@echo off\r\ncd /d "%~dp0qt\\bin"\r\nstart "" "MicArray.exe"\r\n'.encode('ascii'))
lines = [f'{sha(path)}  {path.relative_to(release).as_posix()}'
         for path in sorted(release.rglob('*')) if path.is_file() and path.name != 'SHA256SUMS.txt']
(release / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
print(json.dumps({'release': str(release), 'exe_sha256': sha(exe), 'qt_suites': len(tests)}, ensure_ascii=False))
