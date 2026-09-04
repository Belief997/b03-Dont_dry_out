/// sensor_tool —— sensor_beacon 的桌面配置/采集工具(Windows 优先, 竖窗布局)
///
/// 本文件目前只做"骨架验证": 适配器状态 + 广播扫描列表。目的是打通
/// Flutter → flutter_rust_bridge → btleplug 这条链路, 不是最终 UI。
///
/// ⚠ 三条通路各有不可替代的用途, 后续都要保留:
///   - 串口(P0.06 UART 日志) —— 唯一能长时间连续实时观测的通路
///   - 连接后的命令协议     —— 配置/标定/历史下载, 逐字节可靠, 但要按键开窗
///   - 广播扫描(本文件)     —— 无线快速一瞥, Windows 下丢包严重
///
/// ⚠ 关于"设备搜不到": 固件平时【完全静默】, 只在单击按键后播
///   BLE_BEACON_ADV_EVENTS 个广播事件。所以扫描列表长期空白是正常状态。
///   这条提示必须留在界面上 —— 现有的 web 工具就是这么做的, 别在重写时丢掉。
library;

import 'package:flutter/material.dart';

import 'src/rust/api/ble.dart';
import 'src/rust/frb_generated.dart';

Future<void> main() async {
  await RustLib.init();
  runApp(const SensorToolApp());
}

class SensorToolApp extends StatelessWidget {
  const SensorToolApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'sensor_beacon 工具',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorSchemeSeed: Colors.teal,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: const ScanPage(),
    );
  }
}

class ScanPage extends StatefulWidget {
  const ScanPage({super.key});

  @override
  State<ScanPage> createState() => _ScanPageState();
}

class _ScanPageState extends State<ScanPage> {
  String _adapter = '(未检测)';
  String? _adapterError;
  bool _scanning = false;

  /// 最近收到的广播, 最新的在前。
  ///
  /// Rust 侧已按 (device_id, counter) 去重, 所以这里每条都是一轮 burst,
  /// 不需要再过滤。
  final List<BleAdvEvent> _events = [];

  /// 上限。标定/采集时一次不会看几百条; 200 条足够回溯又不会让列表卡。
  static const _maxEvents = 200;

  @override
  void initState() {
    super.initState();
    _probeAdapter();
  }

  void _probeAdapter() {
    try {
      final name = bleAdapterName();
      setState(() {
        _adapter = name;
        _adapterError = null;
      });
    } catch (e) {
      setState(() {
        _adapter = '(不可用)';
        _adapterError = '$e';
      });
    }
  }

  void _toggleScan() {
    if (_scanning) {
      try {
        bleScanStop();
      } catch (e) {
        _snack('停止扫描失败: $e');
      }
      setState(() => _scanning = false);
      return;
    }

    try {
      final stream = bleScanStart();
      setState(() {
        _scanning = true;
        _events.clear();
      });
      stream.listen(
        (ev) {
          if (!mounted) return;
          setState(() {
            _events.insert(0, ev);
            if (_events.length > _maxEvents) _events.removeLast();
          });
        },
        onError: (Object e) {
          if (!mounted) return;
          setState(() => _scanning = false);
          _snack('扫描出错: $e');
        },
        onDone: () {
          if (!mounted) return;
          setState(() => _scanning = false);
        },
      );
    } catch (e) {
      _snack('启动扫描失败: $e');
    }
  }

  void _snack(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('sensor_beacon 工具'),
        actions: [
          IconButton(
            tooltip: '重新检测适配器',
            onPressed: _probeAdapter,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: Column(
        children: [
          _AdapterCard(name: _adapter, error: _adapterError),
          _ScanControl(
            scanning: _scanning,
            count: _events.length,
            onToggle: _toggleScan,
          ),
          const Divider(height: 1),
          Expanded(
            child: _events.isEmpty
                ? _EmptyHint(scanning: _scanning)
                : ListView.separated(
                    itemCount: _events.length,
                    separatorBuilder: (_, _) => const Divider(height: 1),
                    itemBuilder: (_, i) => _AdvTile(ev: _events[i]),
                  ),
          ),
        ],
      ),
    );
  }
}

class _AdapterCard extends StatelessWidget {
  const _AdapterCard({required this.name, this.error});

  final String name;
  final String? error;

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.all(8),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  error == null ? Icons.bluetooth : Icons.bluetooth_disabled,
                  size: 18,
                  color: error == null ? Colors.tealAccent : Colors.redAccent,
                ),
                const SizedBox(width: 8),
                const Text('适配器',
                    style: TextStyle(fontWeight: FontWeight.bold)),
              ],
            ),
            const SizedBox(height: 6),
            Text(name, style: const TextStyle(fontSize: 12)),
            if (error != null) ...[
              const SizedBox(height: 6),
              Text(
                error!,
                style: const TextStyle(fontSize: 11, color: Colors.redAccent),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _ScanControl extends StatelessWidget {
  const _ScanControl({
    required this.scanning,
    required this.count,
    required this.onToggle,
  });

  final bool scanning;
  final int count;
  final VoidCallback onToggle;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: Row(
        children: [
          Expanded(
            child: FilledButton.icon(
              onPressed: onToggle,
              icon: Icon(scanning ? Icons.stop : Icons.play_arrow),
              label: Text(scanning ? '停止扫描' : '开始扫描'),
            ),
          ),
          const SizedBox(width: 12),
          Text('$count 轮', style: const TextStyle(fontSize: 12)),
        ],
      ),
    );
  }
}

class _EmptyHint extends StatelessWidget {
  const _EmptyHint({required this.scanning});

  final bool scanning;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              scanning ? Icons.sensors : Icons.sensors_off,
              size: 40,
              color: Colors.white24,
            ),
            const SizedBox(height: 16),
            Text(
              scanning ? '扫描中 —— 列表空着是正常的' : '未开始扫描',
              style: const TextStyle(fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            if (scanning)
              const Text(
                // 这段提示不是客套话: 不写的话第一次用的人一定会以为工具坏了。
                '设备平时完全静默, 不播任何广播。\n'
                '请到设备上【单击一次按键】, 它才会播一轮数据。\n\n'
                '⚠ Windows 的 BLE 侦听器约每 1.3 秒才开一次扫描窗口, '
                '而设备一轮只持续 1.5 秒 —— 按一次可能一条都收不到, '
                '多按几次是正常操作。可靠的数据采集要走连接后的记录下载。',
                textAlign: TextAlign.center,
                style: TextStyle(
                    fontSize: 11, color: Colors.white54, height: 1.6),
              ),
          ],
        ),
      ),
    );
  }
}

class _AdvTile extends StatelessWidget {
  const _AdvTile({required this.ev});

  final BleAdvEvent ev;

  @override
  Widget build(BuildContext context) {
    if (!ev.ok) {
      return ListTile(
        dense: true,
        leading: const Icon(Icons.error_outline, color: Colors.orangeAccent),
        title:
            Text(ev.mac ?? '(未知 MAC)', style: const TextStyle(fontSize: 12)),
        subtitle: Text(
          // 解析失败也显示 —— 固件升级到新协议版本后, 这是唯一的线索。
          '解析失败: ${ev.error ?? "未知原因"}\n${_hex(ev.raw)}',
          style: const TextStyle(fontSize: 11, color: Colors.orangeAccent),
        ),
      );
    }

    final idHex = ev.deviceId.toRadixString(16).padLeft(4, '0').toUpperCase();
    final verHex = ev.version.toRadixString(16).padLeft(2, '0');

    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  ev.mac ?? '(未知 MAC)',
                  style: const TextStyle(
                      fontSize: 12, fontWeight: FontWeight.bold),
                ),
              ),
              Text(
                ev.rssi == null ? 'n/a' : '${ev.rssi} dBm',
                style: const TextStyle(fontSize: 11, color: Colors.white54),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Text(
            'ID 0x$idHex  #${ev.counter}  ${ev.battMv} mV  v0x$verHex',
            style: const TextStyle(fontSize: 11, color: Colors.white70),
          ),
          const SizedBox(height: 4),
          // 增益标注直接写在界面上: 三通道灵敏度差 4 倍且载荷不带增益字段,
          // 不标的话很容易把 ch1 的数值跟 ch0/ch2 直接比较。
          _chRow('ch0', ev.ch[0], '增益128'),
          _chRow('ch1', ev.ch[1], '增益 32'),
          _chRow('ch2', ev.ch[2], '增益128'),
        ],
      ),
    );
  }

  Widget _chRow(String name, int v, String gain) {
    return Padding(
      padding: const EdgeInsets.only(top: 2),
      child: Row(
        children: [
          SizedBox(
            width: 34,
            child: Text(name,
                style: const TextStyle(fontSize: 11, color: Colors.white54)),
          ),
          SizedBox(
            width: 90,
            child: Text(
              '$v',
              textAlign: TextAlign.right,
              // 三通道要按列对齐, 等宽字体是必需的, 不是审美选择。
              style: const TextStyle(fontSize: 12, fontFamily: 'Consolas'),
            ),
          ),
          const SizedBox(width: 10),
          Text(gain,
              style: const TextStyle(fontSize: 10, color: Colors.white38)),
        ],
      ),
    );
  }

  static String _hex(List<int> b) =>
      b.map((x) => x.toRadixString(16).padLeft(2, '0')).join(' ').toUpperCase();
}
