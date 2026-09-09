// 集成测试 —— 会真正加载 Rust 动态库, 所以这里才能验证 FRB 桥是通的。
//
// 跑法: flutter test integration_test/simple_test.dart -d windows
//
// ⚠ 不要在这里测"能不能扫到设备": 设备平时完全静默, 只有人去单击按键才播广播,
//   而且 Windows 下收包率天然很低(见 rust/src/ble/mod.rs 的文件头)。
//   那属于需要真硬件在场的手工验证, 不是自动化测试该管的事。
//   本文件只验证"桥通了、适配器能枚举"。

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:sensor_tool/src/rust/api/ble.dart';
import 'package:sensor_tool/src/rust/api/simple.dart';
import 'package:sensor_tool/src/rust/frb_generated.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async => await RustLib.init());

  test('FRB 桥可用', () {
    expect(greet(name: 'Tom'), 'Hello, Tom!');
  });

  test('适配器状态起始为 Idle', () {
    // 还没开始扫描也没连接 —— 必须是 Idle。这条能抓到"全局状态被污染"。
    expect(bleMode(), BleMode.idle);
  });

  test('能读到蓝牙硬件状态', () {
    // 旧版这里要 try/catch —— bleAdapterName() 在没有适配器时抛异常, 得把异常
    // 也算成通过。新 API 不抛了: "没有适配器"是 absent 这个正常返回值, 所以
    // 这条测试简单了一截。
    //
    // ⚠ 断言故意很弱: 本机有没有蓝牙、开没开都是【环境】问题, 不该让测试红。
    //   这条真正验证的只是"这个 FFI 调用能跑通并返回一个合法枚举值"。
    // ⚠ 也正因为断言弱, 必须把实际状态打出来 —— 否则"测试通过"无法区分
    //   "真读到 ready" 与 "这台机器没蓝牙, 读到 absent"。
    final s = bleAdapterStatus();
    // ignore: avoid_print
    print('[蓝牙] 状态 = $s');
    expect(s, isA<BleAdapterStatus>());
  });
}
