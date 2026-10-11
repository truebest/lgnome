**Модульный Codex review lgnome**

**Проверка и исправления, 11 октября 2026 года**

Каждый из 50 пунктов перепроверен по реальным callers и состояниям. Подтвердились описанные ошибки кода, условные риски и пробелы проверок; для всех внесены исправления. Это не означает, что все сценарии наблюдались на физическом ТВ. Исходное разделение на 33 ошибки, 10 рисков и 7 мест для внимания сохранено ниже.

Рабочая ветка: `fix/review-findings-20261011`, исходный HEAD `d9e105829089c8a53d9163a8b36a618cd7da5211`. Исправления выполнены в изолированном worktree. Таблица описывает результат исправлений; исходный аудит ниже относится к старому HEAD и сохраняет исторические строки и сценарии.

Для внешних компонентов проверен upstream:

- **F02 / IronRDP:** проверенный upstream `e4143f26306a6639a889d80c1c484ca69c365a8f` не содержит нужных лимитов [DVC](https://github.com/Devolutions/IronRDP/blob/e4143f26306a6639a889d80c1c484ca69c365a8f/crates/ironrdp-dvc/src/complete_data.rs) и [SVC](https://github.com/Devolutions/IronRDP/blob/e4143f26306a6639a889d80c1c484ca69c365a8f/crates/ironrdp-svc/src/lib.rs). Сохранён существующий fork patch set, добавлены границы до append и forwarding EGFX budget в replay wrapper. Новый доступный pin: `6989bb3a6c0e76c8382e84686b298837000df468`, ветка `fix/lgnome-reassembly-bounds-20261011` существующего форка `truebest/IronRDP`.
- **F04 / webOS CLI:** установленный3.2.4 и проверенный upstream3.2.6 `ed7a6994be443207f3427b19f210f6606f004eb6` по-прежнему [строят single-quoted shell command](https://github.com/webos-tools/cli/blob/ed7a6994be443207f3427b19f210f6606f004eb6/lib/base/luna.js). Продукт отклоняет апострофы до обращения к устройству.
- **F44 / webOS CLI:** тот же upstream по-прежнему [пересоздаёт package directories через mkdirp](https://github.com/webos-tools/cli/blob/ed7a6994be443207f3427b19f210f6606f004eb6/lib/package.js), теряя special mode bits. Продукт восстанавливает1777 в собственном IPK и проверяет результат. CLI не патчился; фактическая потеря sticky bit и исправленный archive mode проверены на собранном ARM IPK.
- LVGL, miniaudio и backend_ndl менять не потребовалось: соответствующие ошибки исправлены в product wiring. Их пины сохранены.

| ID | Результат | Исправление | Регрессионная проверка |
| --- | --- | --- | --- |
| [F01](#f01) | Исправлено | Accept self-signed trust while delegating TLS1.2 and TLS1.3 handshake verification and advertised schemes to rustls ring algorithms | TLS1.2 and TLS1.3 in-memory handshakes accept matching self-signed keys and reject unrelated keys and corrupted signatures |
| [F02](#f02) | Исправлено | Bound DVC messages using per-processor budget (16MiB default, 64MiB graphics); cap SVC messages64MiB; reject oversized declarations and checked pre-append accumulation including compressed sequences; clear failed sequence; replay graphics wrapper forwards larger EGFX budget | u32MAX first fragments rejected before retention; excess fragments and compressed cap crossing rejected; valid sequences and declared full4K RGBA remain accepted |
| [F03](#f03) | Исправлено | Скрипт готовит один локальный snapshot candidate; публикация через PR, тег после merge | release-github: isolated bare remote rejects pushes; no local/remote tag or push |
| [F04](#f04) | Исправлено | Отклонение literal/escaped апострофа до install/launch | deploy-webos: apostrophes, escaped Unicode, benign metacharacters, no device commands on error |
| [F05](#f05) | Исправлено | Persist negotiated KEY_EXCH and condition RC4 checksum consumption on it | Independent OpenSSL RC4/HMAC-MD5 incoming and outgoing vectors for two sequential messages in both KEY_EXCH modes |
| [F06](#f06) | Исправлено | Use current nonzero graphics dimensions, fallback to bitmap dimensions before ResetGraphics; discard old graphics dimensions on reactivation | Actual monitor-layout DVC and refresh/resume rectangle encoding uses1920x1080 while bitmap stays1280x720 |
| [F07](#f07) | Исправлено | Return error for invalid/nonminimal DER, checked arithmetic and1MiB TSRequest limit; cap common transport append before allocation | Fragmented valid headers, invalid tag, indefinite/nonminimal lengths, u32MAX length and append cap; invalid TSRequest rejected before reading payload |
| [F08](#f08) | Исправлено | Consume recovery after active process and request negotiated supported full redraw unless background suppression is latched | Malformed actual FastPath bitmap yields one refresh or suppress/resume pair, none with unsupported capabilities/background suppression; repeated malformed updates yield no further frames |
| [F09](#f09) | Исправлено | Enable rustls tls12 alongside existing std/ring features | TLS1.2-only and TLS1.3-only in-memory server handshake regression tests pass |
| [F10](#f10) | Исправлено | Shared allocation gate checks nonzero dimensions, explicit32-bit checked RGBA byte arithmetic and64MiB canvas budget before callbacks/allocation | Zero, oversized32768x32768/u16MAX and8192x8192 rejected without allocation; normal1280x720 and64MiB boundary validated |
| [F11](#f11) | Исправлено | 30s establishment and15s reactivation deadline, checked by worker and TLS internal socket; active sessions clear read deadline; Stop preserved | Stalled loopback TLS and synthetic stalled X224/CredSSP/reactivation readers expire under short injected deadlines; Stop cancels |
| [F12](#f12) | Исправлено | При несовпадении mode переподключить camera-capable live session через existing native_slot_connect; background заранее arm snapshot/pending fill. До persist HUB отклоняет mode save, если affected camera session ещё connecting. Device/permission saves с прежним per-slot mode не reconnect. | capture-redirect: test_mode_save_restarts_camera_connections:15→30FPS, foreground/background, unchanged save no reconnect; CONNECTING mode-change guard true до persist; прежний формат во время connect разрешён; ACTIVE mode-change guard false; capture_applied_valid=false: прежний per-slot mode не reconnect; новый15→30FPS mode reconnect даже после startup init failure |
| [F13](#f13) | Исправлено | Сохранять negotiation только при первом NULL→nonNULL attachment с разрешённым audio; очистить capture callback state после old worker join. | capture-redirect: test_open_before_session_registration: PCM без второго OPEN; detach+reattach без OPEN не отправляет PCM |
| [F14](#f14) | Исправлено | Списывать camera credit только после принятого SampleError; stop/revoke/teardown по-прежнему явно отменяют credit. | capture-redirect: test_camera_credit_survives_full_mailbox: оба submissions rejected, mailbox освобождён, ровно один ответ без новой SampleRequest |
| [F15](#f15) | Исправлено | При -ESTRPIPE обходить blocking ALSA recover и возвращать terminal read failure в outer retry loop. | audio-input-alsa: injected -ESTRPIPE не вызывает recover; -EPIPE сохраняет recovery; -EAGAIN остаётся nonblocking timeout |
| [F16](#f16) | Исправлено | Absolute write/flush deadline + stop-aware TLS socket wrapper; capture privacy gate продолжает ждать завершения принятых sends. | writes_and_flushes_expire_despite_partial_progress; socket_rechecks_deadline_and_stop_inside_tls_io; existing capture send-permit barrier tests;100 full Rust tests per RDP agent, /tmp/lgnome-fixes-rdp-tests.log |
| [F17](#f17) | Исправлено | Общий focus predicate на grab и input activation; сохранять focus flag на connect completion; regrab только при реально возвращённом фокусе. | native-runtime: ACTIVE arrives backgrounded/unfocused→no grab/no RDP input; SHOWN/RESTORED/DIDFOREGROUND без keyboard focus→no grab; FOCUS_GAINED с actual focus→grab; FOCUS_LOST→input disarmed |
| [F18](#f18) | Исправлено | Сделать handoff outcome явным и при отказе снять streaming_visible, отпустить input и показать UI. | native-runtime: wrapper injects survivor terminal перед реальным handoff guard; после одного supervisor pass no sessions, streaming false, UI visible; следующий tick сохраняет recovery state |
| [F19](#f19) | Исправлено | Перед return из RGBA presentation запросить mixer hide; очистить скрытый drag. | native-runtime: production RGBA present path вызывает hide и очищает dragging |
| [F20](#f20) | Исправлено | Перенести cache routing под video_lock независимо от active; serialise entire replay batch; disarm pending cache до handoff publish. | native-snapshot-replay: concurrent late tail decoder order1,2,3; hook confirms batch lock при second cached AU; late tail after active publication before replay stays cached and quiet gate rejects replay without premature live feed; existing native-rdp-video CTest passed; native-rdp-video: active flip + armed snapshot + recent bitmap refinement сохраняет old-owner decoder |
| [F21](#f21) | Исправлено | source_mark_unavailable больше не двигает read_cursor; смена generation владеет reset | test-audio-pipeline: first_format_published_during_render (включая wrap clock) |
| [F22](#f22) | Исправлено | Per-slot recent H264 seen/time/epoch под video_lock; reset после join; bitmap classification entirely under lock, active armed snapshot recent refinement preserves old decoder | test-native-rdp-video: background refinement, timeout, epoch, bitmap-only, clock wrap; test-native-rdp-video: deterministic demotion at mutex acquire; active flip while snapshot armed preserves decoder |
| [F23](#f23) | Исправлено | Проверка UI byte limits до store/save/connect; invalid draft не изменяет slot_values | test-ui-preconnect-input: real LVGL password512bytes rejected, saved password unchanged, 510 valid, username/domain overflow |
| [F24](#f24) | Исправлено | Общий release-all RESET на removal и terminal poll failure, queued history discarded | test-input-evdev-disconnect: real removal after mouse/Shift DOWN and another-device queued event; RESET and subsequent events; test-input-evdev-disconnect: real terminal poll failure publishes RESET+wake and releases devices; test-native-pointer-control: real native_flush_held_inputs emits Shift/LEFT UP once |
| [F25](#f25) | Исправлено | Оба устройства останавливаются на первом событии другого класса в общей SDL FIFO | test-ui-preconnect-input: real LVGL timers click→text и text→click |
| [F26](#f26) | Исправлено | Drag clear на focus/background и motion без LEFT; focus/background gate на updates | test-native-runtime: capture agent выполнил real event tests focus loss/regain, unpressed motion, WILL/DIDBACKGROUND |
| [F27](#f27) | Исправлено | Cursor apply/warp gated by streaming/mixer/focus/background; stale pending warp discarded; producer tests atomic input.active, real input stop after held UP disarms and clears mailbox under video_lock | test-native-pointer-control: real consumer and producer callback, all four ownership gates and resume; test-native-pointer-control: no drain during disarmed HUB/mixer, subsequent resume cannot replay those positions; test-native-runtime: actual native_stop_streaming_input clears pending warp and disarms input on focus lost |
| [F28](#f28) | Исправлено | Mutex protects cache/edit generation; GET only accepted for matching generation without pending SET; SET pending blocks GET | test-luna-volume-order: real callback/coalescing paths, delayed GET during SET and after SET replies; next +3 gives39; nested fresh GET accepted; test-luna-volume-order: rejected SET followed by fresh GET reconciles actual volume; test-luna-volume: flat+nested payloads; fractional/wrong/nonnamed nested fields rejected |
| [F29](#f29) | Исправлено | Ёмкость11, последний /tmp fallback сохранён | config-storage: real collector count11 and last /tmp |
| [F30](#f30) | Исправлено | Убран cache; выбор и private dirfd validation при каждом Save | config-storage: parent replaced with symlink; unrelated target unchanged |
| [F31](#f31) | Исправлено | Discovered reads: nofollow, regular file, euid ownership, no group/other write | config-storage: chmod healing plus symlink and writable planted-file rejection |
| [F32](#f32) | Исправлено | Whole root object validator before transactional apply; embedded fileNUL rejected | settings-json: malformed root documents; config-storage: invalid/NUL high candidate falls back |
| [F33](#f33) | Исправлено | Save выбирает highest writable location, но отказывает если его скроет higher valid readable file; миграция в более высокий доступный persistent путь сохранена | config-storage: read-only high path cannot mask successful Save to low path; lower fallback migrated to newly available higher writable candidate |
| [F34](#f34) | Исправлено | C/file envelope128KiB, bounded save; deploy canonical Node JSON and120KiB launch budget before device commands | settings-json: worstescaped full model >16KiB roundtrip; deploy-webos: oversizedfile/params and19000 JS-expanded floats rejected before install, smallfloat accepted |
| [F35](#f35) | Исправлено | UTF8 encoding and surrogate pair validation in shared string scanner | settings-json: Latin/Cyrillic/emoji, lone and malformed surrogates |
| [F36](#f36) | Исправлено | Escaped NUL rejected; file literalNUL rejected before parse | settings-json: embedded escaped NUL unchanged transaction; config-storage: literalNUL invalid file |
| [F37](#f37) | Исправлено | Full JSON number grammar and complete bool/integer token readers | settings-json: truefalse,1abc,1.5,1e3,leadingzeros and valid boundary scalars |
| [F38](#f38) | Исправлено | Lookup только непосредственных полей, структурный skip неизвестных values | settings-json: nested globals/fake string keys; luna-volume supports explicit volumeStatus object |
| [F39](#f39) | Исправлено | fflush/fsync file→renameat→fsync parent with errors returned | config-storage: injected pre-rename file fsync failure preserves old; directory fsync failure reported |
| [F40](#f40) | Исправлено | Host fixture compiles production storage with LGNOME_TARGET_WEBOS and fake SDL pref-path only | config-storage CTest: real lifecycle, precedence, permissions, malformed fallback, symlink, durability |
| [F41](#f41) | Исправлено | Full webrdp suite in bothCI; external reassembly/replay regressions added | 100full Rust tests;66IronRDP tests; workflows and docs updated |
| [F42](#f42) | Исправлено | Script checks requested version and appID against main and verified IPK | release-github: source and IPK mismatch before branch/tag creation |
| [F43](#f43) | Исправлено | open then dup2 only after successful open; error keeps stderr | native-logging: invalid destination leaves fd2 open; successful log redirect persists marker |
| [F44](#f44) | Исправлено | Restore1777 in own IPK payload; verify extraction preserves archive modes | deploy-webos: badmode rejected and repaired archive preserved payload; actual ARM IPK verified1777 |
| [F45](#f45) | Исправлено | Resolve through locked cargo metadata; explicit archive and skip-build retain original behavior | Full ARM product build with custom CARGO_TARGET_DIR used matching archive |
| [F46](#f46) | Исправлено | Read cursor first, write cursor second; observed frames bounded by capacity | test-audio-pipeline: deterministic interleave hook between cursor loads advances producer and consumer |
| [F47](#f47) | Исправлено | KP0 handled separately, KP1..KP9 range follows real SDL2 enum order | test-ui-preconnect-input: all ten fallback digits and TEXTINPUT duplicate suppression |
| [F48](#f48) | Исправлено | Existing video_lock serializes publication and snapshot/clear; active gate repeated under lock | test-native-pointer-control: actual callback publisher and SDL consumer 100000 concurrent updates, every observed x==y |
| [F49](#f49) | Исправлено | Validate all known valued options before config load/apply | settings-json: every valued CLI missing/next--option rejected |
| [F50](#f50) | Исправлено | Validate selected startup slot before connecting; green remains default | settings-json: configured red validates with invalid green; omitted selection retains green |

Проверки локального исправленного дерева: Rust format и native syntax/logging policy проходят;100 полных Rust tests,11 DVC/SVC tests,55 capture-replay tests и84 host CTests с Opus прошли. Полная ARM webOS-сборка выполнена с отдельным `CARGO_TARGET_DIR`; проверены product identity, Rust/NDL/SDL wiring, отсутствие сохранённых settings и mode1777 в IPK. Финальный повтор host/Rust/syntax checks и ARM cross-build после всех source fixes завершился успешно. IPK SHA-256: `fb7d6ad5b0d6fc0f02ac22ac0551e29672142ba6231a5113cf78339ce9c1cb0a`. Версия0.5.5 сохранена: это development fix batch без release. Позднее выполненный TV deployment описан ниже.

Каждый саб-агент повторно запустил буквальный локальный `codex review` своего исправленного участка. Все8 запусков (первый проход и focused follow-up для каждого из4review owners) завершились с exit0. Найденные в первых проходах регрессии исправлены и покрыты тестами: EGFX replay budget forwarding, bitmap demotion, late pointer warp, camera save while connecting и deployment size после JavaScript reserialization. Финальные проходы не нашли actionable defects в своих scopes. RDP агент отдельно проверил connected C↔Rust shutdown/capture/pointer/deadline integration и не нашёл новой регрессии. Это локальные CLI reviews.

| Исправленный участок | Первый проход | Чистый follow-up |
| --- | --- | --- |
| RDP / transport / auth / IronRDP | [log](/tmp/lgnome-fixes-rdp-codex-review.log) | [log](/tmp/lgnome-fixes-rdp-codex-followup.log) |
| Capture / runtime / switching | [log](/tmp/lgnome-fixes-capture-review.log) | [log](/tmp/lgnome-fixes-capture-review-final.log) |
| Playback / UI / input / volume | [log](/tmp/lgnome-fix-playback-codex-review.log) | [log](/tmp/lgnome-fix-playback-codex-review-round2.log) |
| Config / storage / tooling | [log](/tmp/lgnome-fixes-root-codex-review.log) | [log](/tmp/lgnome-fixes-root-codex-followup.log) |

Логи финальных проверок: [CTest84](/tmp/lgnome-all-fixes-ctest.log), [Rust100](/tmp/lgnome-all-fixes-rust.log), [IronRDP66](/tmp/lgnome-all-fixes-ironrdp.log), [ARM build/IPK](/tmp/lgnome-all-fixes-webos.log). Regression fixtures входят в репозиторий; временные logs не нужны для их повторного запуска.

Не выполнялись live Windows/grd interop matrix, suspend на реальном ALSA driver, destructive OOM/MITM или slow-network soak. F12 требует короткого reconnect live camera sessions при изменении геометрии/FPS; Save отклоняется пока затронутая сессия подключается. TLS self-signed trust остаётся разрешённым, а владение ключом handshake теперь проверяется. DNS и отдельный TCP connect остаются синхронными; рабочие protocol phases и write/flush получили deadlines.

**Проверка на ТВ после исправлений**

11 октября 2026 года сборка из `main` `eb82ffc` установлена поверх существующего `com.truebest.lgnome.native` без удаления приложения. SHA-256 установленного `bin/lgnome-native` совпал с новым ARM-пакетом: `481deab2766ab490d3609647a7887cf7769496c64c422d5ee5a51fda8963f2eb`. Свежий startup log подтвердил `loaded persisted config` и NDL initialization (`sdk=8.3.0`, `v2-window-id-init`). Приложение осталось запущенным. Пакет не содержал settings/config files. Резервная копия сохранённых профилей через Developer Mode SSH была недоступна; восстановление из backup не проверялось.

Первая установка выявила регрессию исправления F44: GNU `ar r` переписал имена и metadata всех членов IPK, добавив `/` к исходным именам CLI. Локальные `ar`/`tar` и verifier принимали архив, но TV installer вернул `IPK Extraction Failure`. Исправленный пакет с исходными ar headers и тем же `data.tar.gz` установился успешно; это изолирует причину от gzip/tar contents. Helper теперь заменяет только data payload и поле его размера, сохраняя остальные headers, member order, padding и payloads. Regression fixture проверяет исходный CLI format и сохранение остальных данных. SHA-256 установленного совместимого IPK: `25803503ce06fc183c02ec732c93b90adb911e2202b688f4b4bef28fbffde4fd`.

Исправление упаковщика проверено отдельным `codex review --uncommitted`: exit0, actionable findings нет ([log](/tmp/lgnome-ipk-archive-codex-review.log)). Deploy regression, 288 переходов размера/padding и malformed cases, end-to-end gz/xz/bz2/plain-tar, а также исправление реального CLI IPK с последующим product verifier прошли.

Ограничение F44 для обновления существующей установки: архив содержит пустой `settings/` с mode1777, но TV installer сохранил старый каталог с mode0777 и приватный per-UID directory с mode0700. SSH-пользователь не смог изменить mode старого каталога. Поэтому исправление archive mode подтверждено, миграция прав существующего каталога на ТВ не подтверждена и требует доступа его владельца. Startup/config/NDL smoke пройден; длительная RDP-сессия, камера/микрофон и переключение четырёх профилей на этом бинарнике не проверялись.

**Дополнительная проверка IronRDP для 0.5.6**

При выполнении запроса сопровождающего [PR1789](https://github.com/Devolutions/IronRDP/pull/1789#issuecomment-6059143770) повторно проверены автоматические замечания. В текущем product pin обнаружен остаточный дефект: после16 ошибок Progressive эвристика глобально очищала tile state разных поверхностей. Отказ от такого сброса сохраняет CONTEXT/TILE_UPGRADE state; cumulative diagnostics и уведомление после32 последовательных ошибок сохранены. Это отдельное уточнение ранее проверенного codec recovery, а не изменение лимитов reassembly F02.

Исправление перенесено на точный проверенный pin, без обновления product base до нового upstream. Новый fork pin: `8bfef9fe62bd9d936ba5f253ed3fd0ccb668b49c`, ветка `fix/lgnome-egfx-drop-state-20261011`. Прошли55 EGFX,55 capture-replay,8 DVC и3 SVC tests (121), включая реальные продолжения TILE_UPGRADE разных surfaces после ошибок и32-error notification. EGFX tests включены в оба product CI. Старые результаты smoke и SHA-256 выше относятся к предшествующему binary; финальный0.5.6 package проверяется отдельно.

**Исходный аудит**

Дата: 10 октября 2026 года, America/Los_Angeles. Проверенный HEAD: `d9e105829089c8a53d9163a8b36a618cd7da5211`, ветка `fix/audio-cadence-buffer`. Аудит учитывает текущие изменения audio cadence, вошедшие в этот HEAD.

Код разделён на участки для независимого review. Физическая структура исходников не менялась. Три саб-агента проверили по два участка; основной агент проверил конфигурацию и свёл результаты. Для каждого участка выполнен отдельный локальный `codex review`; все семь запусков завершились с exit code 0. Это означает завершение review, а не отсутствие ошибок. Выводы CLI перепроверены по callers, состояниям и доступным воспроизведениям; дубли удалены.

Всего **50 находок**: **P1 — 4, P2 — 41, P3 — 5**. По характеру: ошибки — 33, риски — 10, места для внимания — 7. P1 требует первоочередного исправления; P2 — следующий приоритет; P3 — ограниченный дефект. Номер не означает, что сценарий часто встречается.

«Высокая уверенность» относится к описанному дефекту кода или воспроизведённому чередованию. Она не подтверждает частоту на TV. Риски и места для внимания содержат условия и ограничения; hardware/network последствия, которые не измерены, явно отмечены. Синтетические probes не использовали сохранённые пользовательские credentials.

**Разделение review**

| Участок | Reviewer | Охват | Находки | Локальный CLI log |
| --- | --- | --- | --- | --- |
| Rust / RDP и безопасность | `/root/review_rdp` | webrdp-min: transport, connection/activation, active I/O, EGFX, input, CredSSP/NTLM/crypto/DER, RDPSND и DVC callers | [F01](#f01), [F05](#f05), [F06](#f06), [F07](#f07), [F08](#f08), [F09](#f09), [F10](#f10), [F11](#f11) | [log](/tmp/lgnome-review-rdp.log) |
| Camera / microphone / capture | `/root/review_capture` | C capture lifecycle/workers, V4L2/H.264 stream, ALSA, Rust RDPECAM/RDPEAI, media mailbox и входящая сборка virtual channels | [F02](#f02), [F12](#f12), [F13](#f13), [F14](#f14), [F15](#f15), [F16](#f16) | [log](/tmp/lgnome-review-capture.log) |
| Runtime / sessions / HUB / switch | `/root/review_capture` | Lifecycle, loop phases, session supervision, focus, input ownership, HUB actions, handoff/recovery и snapshot replay | [F17](#f17), [F18](#f18), [F19](#f19), [F20](#f20) | [log](/tmp/lgnome-review-runtime.log) |
| Video / audio playback / NDL | `/root/review_playback` | RDP media callbacks, audio pipeline/jitter/Opus, H.264/Annex-B/snapshots, bitmap/RGBA, NDL adapters и backend_ndl | [F21](#f21), [F22](#f22), [F46](#f46) | [log](/tmp/lgnome-review-playback.log) |
| UI / input / cursor / mixer | `/root/review_playback` | LVGL preconnect/profile/capture/HUB, SDL и evdev, cursor mailboxes, mixer и Luna volume | [F23](#f23), [F24](#f24), [F25](#f25), [F26](#f26), [F27](#f27), [F28](#f28), [F47](#f47), [F48](#f48) | [log](/tmp/lgnome-review-ui.log) |
| Settings / JSON / CLI / storage | `/root` | Settings model/defaults, validation, JSON codec/scanner, launch overrides, path selection и persisted lifecycle | [F29](#f29), [F30](#f30), [F31](#f31), [F32](#f32), [F33](#f33), [F34](#f34), [F35](#f35), [F36](#f36), [F37](#f37), [F38](#f38), [F39](#f39), [F40](#f40), [F49](#f49), [F50](#f50) | [log](/tmp/lgnome-review-config.log) |
| Build / CI / package / deploy / release / logging | `/root/review_rdp` | tools/, CMake/Cargo integration, CI, IPK verification, deployment/release scripts, provenance, logging и webOS platform | [F03](#f03), [F04](#f04), [F41](#f41), [F42](#f42), [F43](#f43), [F44](#f44), [F45](#f45) | [log](/tmp/lgnome-review-tooling.log) |

Логи и probes в `/tmp` — временные материалы текущей проверки. Сценарии, существенные результаты, места и предложенные regression tests приведены ниже, чтобы основной отчёт оставался полезным после удаления этих материалов.

**Находки**

**Приоритет P1**

<a id="f01"></a>

**F01. [P1] TLS принимает подпись рукопожатия от постороннего ключа**

Участок: Rust / TLS. Тип: риск. Уверенность: высокая.

Места: [webrdp-min/src/native/transport.rs:98](/home/truebest/psrc/gnomecast/webrdp-min/src/native/transport.rs:98), [webrdp-min/src/native/transport.rs:107](/home/truebest/psrc/gnomecast/webrdp-min/src/native/transport.rs:107).

**Сценарий и последствия.** Посредник копирует сертификат настоящего RDP-сервера, подписывает TLS-рукопожатие собственным ключом и ретранслирует CredSSP к серверу. Обе стороны видят один публичный ключ сертификата, поэтому проверка привязки CredSSP не обнаруживает подмену; последующий RDP-трафик проходит через посредника.

**Проверка.** Оба verify_tls*_signature безусловно возвращают HandshakeSignatureValid::assertion(). Изолированная проверка оригинальных callbacks принимает неверную подпись, а штатный ring verifier её отклоняет. Отдельный TLS1.3 handshake с намеренно несовпадающими сертификатом и private key успешно завершился. Это нарушение доказательства владения ключом, независимо от намеренно разрешённых self-signed сертификатов.

**Минимальное исправление.** Использовать rustls::crypto::verify_tls12_signature и verify_tls13_signature с алгоритмами ring provider; получать supported_verify_schemes от того же provider.

**Тест.** TLS1.2/1.3: корректный self-signed сертификат и правильная подпись принимаются, подпись постороннего ключа отклоняется.

**Ограничение проверки.** Полная сетевая MITM-атака с настоящим RDP-сервером не выполнялась; не утверждается раскрытие NTLM-sealed authInfo.

<a id="f02"></a>

**F02. [P1] Сервер может исчерпать память через неограниченную сборку DVC/SVC сообщений**

Участок: Capture / Rust inbound virtual channels. Тип: ошибка. Уверенность: высокая.

Места: [third_party/IronRDP/crates/ironrdp-dvc/src/complete_data.rs:42](/home/truebest/psrc/gnomecast/third_party/IronRDP/crates/ironrdp-dvc/src/complete_data.rs:42), [third_party/IronRDP/crates/ironrdp-dvc/src/complete_data.rs:61](/home/truebest/psrc/gnomecast/third_party/IronRDP/crates/ironrdp-dvc/src/complete_data.rs:61), [third_party/IronRDP/crates/ironrdp-svc/src/lib.rs:399](/home/truebest/psrc/gnomecast/third_party/IronRDP/crates/ironrdp-svc/src/lib.rs:399), [third_party/IronRDP/crates/ironrdp-svc/src/lib.rs:409](/home/truebest/psrc/gnomecast/third_party/IronRDP/crates/ironrdp-svc/src/lib.rs:409), [webrdp-min/src/native/connection.rs:164](/home/truebest/psrc/gnomecast/webrdp-min/src/native/connection.rs:164).

**Сценарий и последствия.** На соединении с capture DVC недоверенный сервер объявляет DATA_FIRST length=u32::MAX и продолжает посылать обычные небольшие фрагменты. CompleteData удерживает их до объявленного размера без продуктового предела; до RDPECAM/RDPEAI handler и его проверок дело не доходит. Внешний SVC accumulator также принимает большой declared length и расширяет Vec без cap. Постепенно память ТВ исчерпывается, Rust allocation failure может завершить приложение.

**Проверка.** Ручная проверка реального caller DynamicVirtualChannel::process, обоих accumulators и connection registration. CLI probe /tmp/lgnome-capture-audit-20261010/dvc_reassembly_probe.rs использует реальные PDU/CompleteData: announced=4294967295, accepted/buffered=8388840 без dispatch. Второй reviewer независимо воспроизвёл ту же проблему на 16 MiB.

**Минимальное исправление.** Ввести обоснованные лимиты входящих сообщений до append и checked pre-append accumulation в SVC; для DVC использовать предел по типу канала/назначению либо общий безопасный memory budget. Не переносить outgoing camera 4 MiB на все DVC: RDPEGFX/bitmap имеют другую нагрузку. При превышении очистить sequence и вернуть protocol error до дальнейшей аллокации.

**Тест.** DATA_FIRST с u32::MAX должен отклоняться до накопления; фрагменты, переходящие соответствующий cap, также отклоняются до append. Проверить SVC fragmented/compressed sequences и сохранение допустимых больших RDPEGFX/bitmap updates.

**Ограничение проверки.** OOM процесса и память физического ТВ не доводились до отказа; проверена реальная неограниченная аллокация на безопасном host размере. Проблема общая для virtual channels, не только capture.

<a id="f03"></a>

**F03. [P1] Release script создаёт тег до неатомарного push в защищённый main**

Участок: Tooling / release. Тип: риск. Уверенность: высокая.

Места: [tools/release-github.sh:73](/home/truebest/psrc/gnomecast/tools/release-github.sh:73), [tools/release-github.sh:74](/home/truebest/psrc/gnomecast/tools/release-github.sh:74).

**Сценарий и последствия.** Release пытается напрямую обновить защищённый main вместе с новым тегом. Сервер отклоняет branch ref, но может принять tag ref: публичный version tag указывает на неслитый snapshot, а локальный тег также мешает повторному запуску.

**Проверка.** В изолированном bare Git repository update hook отклонил refs/heads/main. Такой же двух-ref push вернулся с ошибкой, main остался прежним, но refs/tags/v... появился на candidate commit. Это локальная filesystem simulation; внешних push не было.

**Минимальное исправление.** Создавать release candidate, проводить PR через проверки и review, сначала merge, затем ставить тег на полученный main commit и создавать Release с проверенным IPK; обновить инструкцию скрипта.

**Тест.** При отказе main update ни локально/удалённо опубликованный release tag не должен остаться; успешный тег указывает на уже слитый snapshot.

<a id="f04"></a>

**F04. [P1] Escaped apostrophe в explicit config снова становится shell injection в ares-launch**

Участок: Tooling / deployment. Тип: риск. Уверенность: высокая.

Места: [tools/deploy-native-webos.sh:90](/home/truebest/psrc/gnomecast/tools/deploy-native-webos.sh:90), [tools/deploy-native-webos.sh:91](/home/truebest/psrc/gnomecast/tools/deploy-native-webos.sh:91).

**Сценарий и последствия.** Explicit --config содержит username O'Reilly: Python заменяет апостроф на \u0027, но pinned ares-launch JSON.parse восстанавливает символ и luna.js включает JSON.stringify(params) в одинарные shell quotes. Обычный username ломает launch; специально подготовленная строка может добавить команду, исполняемую от TV SSH пользователя.

**Проверка.** Перепроверены реальные исходники pinned @webos-tools/cli3.2.4: /home/truebest/.local/lib/node_modules/@webos-tools/cli/bin/ares-launch.js:198–210 (getParams, JSON.parse), /home/truebest/.local/lib/node_modules/@webos-tools/cli/lib/base/luna.js:53 (single-quoted remote shell command). Вызван настоящий luna.send с fake session; локальная /bin/sh simulation дала syntax error для O'Reilly и исполнила только синтетический harmless printf для crafted value. Recorder test заканчивается раньше опасной CLI boundary.

**Минимальное исправление.** Передавать launch params через транспорт без shell reserialization; пока он не исправлен, отклонять апострофы в explicit config до ares-launch с ясной ошибкой.

**Тест.** Прогнать реальные pinned getParams/luna.send с fake session: обычные и crafted апострофы не ломают command и не создают отдельные команды.

**Ограничение проверки.** Реальный TV, SSH, ares launch/deploy не использовались; execution воспроизведён только локально в fake session.

**Приоритет P2**

<a id="f05"></a>

**F05. [P2] NTLM всегда шифрует checksum, даже без negotiated KEY_EXCH**

Участок: Rust / NTLM. Тип: ошибка. Уверенность: высокая.

Места: [webrdp-min/src/ntlm.rs:338](/home/truebest/psrc/gnomecast/webrdp-min/src/ntlm.rs:338), [webrdp-min/src/ntlm.rs:362](/home/truebest/psrc/gnomecast/webrdp-min/src/ntlm.rs:362).

**Сценарий и последствия.** Сервер не выставляет KEY_EXCH. Authenticate корректно исключает флаг и использует derived session key, но encrypt/decrypt всё равно пропускают HMAC checksum через RC4. Совместимый сервер отклоняет pubKeyAuth, а клиент отклоняет правильную серверную подпись; CredSSP не завершается при верных учётных данных.

**Проверка.** Изолированный probe оригинального ntlm.rs показал неправильный исходящий checksum; второй независимый probe также показал отказ принимать правильный server seal. [MS-NLMP 3.4.4.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-nlmp/a92716d5-d164-4960-9e15-300f4eef44a8) требует открытый checksum при KEY_EXCH=0.

**Минимальное исправление.** Сохранить negotiated KEY_EXCH и шифровать checksum только при установленном флаге; без флага не продвигать RC4 для checksum.

**Тест.** Независимые исходящие и входящие MS-NLMP seal vectors для KEY_EXCH=0 и KEY_EXCH=1.

<a id="f06"></a>

**F06. [P2] Refresh и resume используют старые размеры после EGFX ResetGraphics**

Участок: Rust / graphics control. Тип: ошибка. Уверенность: высокая.

Места: [webrdp-min/src/native/active_io.rs:205](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:205), [webrdp-min/src/native/active_io.rs:218](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:218), [webrdp-min/src/native/active_io.rs:243](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:243), [webrdp-min/src/native/active_io.rs:266](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:266).

**Сценарий и последствия.** Начальная активация 1280×720, затем EGFX ResetGraphics меняет рабочий стол на 1920×1080 без DeactivateAll. GfxState и C callback получают новые размеры, DecodedImage остаётся старым. RequestRefresh повторно отправляет monitor layout 1280×720 и может вернуть сервер к старому размеру; refresh/resume rectangle тоже покрывает старую область.

**Проверка.** Probe с оригинальным NativeGfxHandler подтвердил graphics=1920×1080 при DecodedImage=1280×720. В layout и прямоугольниках читается только image; его пересоздают лишь при RDP-реактивации.

**Минимальное исправление.** Для control PDU использовать актуальные ненулевые gfx.graphics_width/height, с fallback на DecodedImage до первого ResetGraphics.

**Тест.** ResetGraphics без DeactivateAll, затем RequestRefresh и resume: monitor layout и все rectangle должны иметь новый размер.

**Ограничение проверки.** Изменение разрешения на физическом TV не воспроизводилось.

<a id="f07"></a>

**F07. [P2] Некорректный или огромный TSRequest не ограничивает входной буфер**

Участок: Rust / CredSSP framing. Тип: риск. Уверенность: высокая.

Места: [webrdp-min/src/native/transport.rs:50](/home/truebest/psrc/gnomecast/webrdp-min/src/native/transport.rs:50), [webrdp-min/src/native.rs:291](/home/truebest/psrc/gnomecast/webrdp-min/src/native.rs:291), [webrdp-min/src/native.rs:313](/home/truebest/psrc/gnomecast/webrdp-min/src/native.rs:313).

**Сценарий и последствия.** TLS-peer объявляет TSRequest длиной несколько гигабайт или начинает поток с 31 00 / 30 80. Огромная длина принимается, а неверный DER префикс трактуется как незавершённый header. read_ts_request продолжает читать в Vec без лимита, позволяя исчерпать память до аутентификации.

**Проверка.** Оригинальный parser принял header 30 84 7f ff ff ff с total=2147483653; неверный tag и indefinite DER дали None, как неполные данные. read_more безусловно extend_from_slice. Сумма header+length также вычисляется без checked arithmetic для ARM32.

**Минимальное исправление.** Различать incomplete и invalid через Result, проверять checked_add и лимит общего размера TSRequest до дальнейшего чтения.

**Тест.** Фрагментированный корректный header, неправильный tag, indefinite DER, чрезмерная длина и overflow на 32-битном размере.

**Ограничение проверки.** Реальный OOM и 32-битный запуск не выполнялись.

<a id="f08"></a>

**F08. [P2] Native worker не потребляет запрос восстановления повреждённого bitmap**

Участок: Rust / bitmap recovery. Тип: ошибка. Уверенность: высокая.

Места: [webrdp-min/src/native/active.rs:88](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active.rs:88).

**Сценарий и последствия.** Patched IronRDP пропускает повреждённый Fast-Path bitmap, возвращает Ok и выставляет pending bitmap recovery. Native worker не забирает флаг; если дальше меняются другие области, пропущенные пиксели остаются устаревшими до внешней перерисовки.

**Проверка.** Probe оригинального ActiveStage с malformed bitmap получил успешный process без непустого recovery response и take_bitmap_recovery_request()==true. В native нет вызова этого метода; desktop client IronRDP отдельно потребляет флаг и вызывает request_full_redraw.

**Минимальное исправление.** После active.process потреблять recovery request и отправлять существующий запрос полной перерисовки с учётом negotiated capabilities и background suppression.

**Тест.** Повреждённый bitmap приводит к одному поддерживаемому recovery PDU; повторные ошибки не создают неограниченный поток запросов.

<a id="f09"></a>

**F09. [P2] Сборка исключает TLS1.2 и не подключается к TLS1.2-only серверу**

Участок: Rust / TLS compatibility. Тип: ошибка. Уверенность: высокая.

Места: [webrdp-min/Cargo.toml:60](/home/truebest/psrc/gnomecast/webrdp-min/Cargo.toml:60), [webrdp-min/src/native/connection.rs:217](/home/truebest/psrc/gnomecast/webrdp-min/src/native/connection.rs:217).

**Сценарий и последствия.** RDP endpoint поддерживает TLS1.2, но не TLS1.3. Native client предлагает только TLS1.3 и завершает security upgrade ошибкой до CredSSP.

**Проверка.** Cargo rustls default-features=false включает только std/ring; cargo tree -e features -i rustls подтверждает отсутствие tls12. Probe с текущей собранной библиотекой вывел DEFAULT_VERSIONS=[TLSv1_3]. README обещает поддержку встроенного Windows RDP без такого ограничения.

**Минимальное исправление.** Включить rustls feature tls12 вместе с криптографической проверкой handshake signatures.

**Тест.** TLS1.2-only сервер успешно завершает security upgrade; отдельный TLS1.3 сценарий также проходит.

**Ограничение проверки.** Живые Windows endpoint и конкретные версии ОС в этой проверке не использовались.

<a id="f10"></a>

**F10. [P2] Размер canvas от сервера попадает в неограниченное выделение памяти**

Участок: Rust / negotiated canvas. Тип: риск. Уверенность: высокая.

Места: [webrdp-min/src/native/active.rs:52](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active.rs:52), [webrdp-min/src/native/active_io.rs:311](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:311), [third_party/IronRDP/crates/ironrdp-session/src/image.rs:147](/home/truebest/psrc/gnomecast/third_party/IronRDP/crates/ironrdp-session/src/image.rs:147).

**Сценарий и последствия.** Demand Active Bitmap capability содержит чрезмерные u16 width/height. Connector передаёт их без проверки, native вызывает DecodedImage::new. Для 32768×32768 RGBA произведение равно 2^32: на ARM32 release оно оборачивается в ноль, debug паникует; другие чрезмерные размеры требуют огромной памяти и могут завершить приложение.

**Проверка.** Прослежен caller от connection_activation.rs:280–286 к обеим native allocations. DecodedImage::new вычисляет usize width*height*bytes_per_pixel без checked_mul или бюджета. Изолированное вычисление ARM32 release арифметики для указанного размера дало 0.

**Минимальное исправление.** Перед начальной активацией и реactivation проверять ненулевые dimensions, checked byte count и разумный canvas memory budget; возвращать NativeError вместо allocation.

**Тест.** Нулевые, обычные и предельные server bitmap dimensions; oversized и ARM32 overflow отклоняются до callbacks/allocation.

**Ограничение проверки.** Намеренный огромный allocation и выполнение ARM32 бинарника не производились.

<a id="f11"></a>

**F11. [P2] Установление и реактивация не имеют общего deadline**

Участок: Rust / connection lifecycle. Тип: место для внимания. Уверенность: высокая.

Места: [webrdp-min/src/native.rs:316](/home/truebest/psrc/gnomecast/webrdp-min/src/native.rs:316), [webrdp-min/src/native/connection.rs:232](/home/truebest/psrc/gnomecast/webrdp-min/src/native/connection.rs:232), [webrdp-min/src/native/active_io.rs:300](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:300).

**Сценарий и последствия.** Peer принимает TCP и остаётся открытым, но не заканчивает X.224, TLS, CredSSP или реактивацию. 10ms socket poll timeout каждый раз считается успешным ожиданием, поэтому слот остаётся connecting или frozen без автоматического завершения/восстановления.

**Проверка.** CONNECT_TIMEOUT ограничивает только TcpStream::connect_timeout. read_more возвращает Ok для каждого timeout; read_hinted_pdu/read_ts_request/upgrade_tls/drive_reactivation имеют циклы только с cancellation, без общего срока. Это конкретный непокрытый stalled-peer сценарий.

**Минимальное исправление.** Задать deadline для establishment и reactivation, сохранив короткий poll для Stop; обычные простаивающие active sessions не ограничивать этим deadline.

**Тест.** Peer держит соединение открытым и не завершает каждый этап: по deadline возникает диагностируемая ошибка; Stop продолжает быстро отменять ожидание.

**Ограничение проверки.** Live stalled-peer test не запускался; вывод основан на полном control-flow trace.

<a id="f12"></a>

**F12. [P2] Сохранение нового режима камеры оставляет работающий RDPECAM bridge со старым media type**

Участок: Capture / camera configuration. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_capture_glue.c:72](/home/truebest/psrc/gnomecast/native/src/native_capture_glue.c:72), [native/src/native_hub_actions_profiles.c:34](/home/truebest/psrc/gnomecast/native/src/native_hub_actions_profiles.c:34), [webrdp-min/src/native.rs:138](/home/truebest/psrc/gnomecast/webrdp-min/src/native.rs:138), [webrdp-min/src/native/rdpecam/bridge.rs:18](/home/truebest/psrc/gnomecast/webrdp-min/src/native/rdpecam/bridge.rs:18), [native/src/capture_camera_worker.c:36](/home/truebest/psrc/gnomecast/native/src/capture_camera_worker.c:36).

**Сценарий и последствия.** Камера работает 640x480@15. Через HUB сохранить другой поддержанный FPS/размер. C перезапускает producers и переобъявляет устройство, но Rust CameraBridge сохраняет исходные параметры соединения. Сервер выбирает старый media type; C worker требует новый mode и не находит подходящий stream. Принятый sample credit остаётся без AU и SampleError до reconnect.

**Проверка.** Прослежены HUB save, reconfigure, immutable Rust bridge, media-type advertisement/StartStreams и exact-mode C predicate. /tmp/lgnome-capture-mode-repro.c воспроизводит opened=0 packets=0 errors=0 outstanding_credit=1 после @15→@30 и текущего старого Rust callback. CLI review независимо подтвердил finding.

**Минимальное исправление.** При geometry/FPS change пересоздать затронутые camera-enabled sessions/bridge либо добавить согласованное live обновление Rust media type с retirement прежней generation. Availability remove/add сама по себе режим не обновляет.

**Тест.** Живой camera-enabled слот @15 → HUB save @30 → device negotiation → новый поток @30 должен ответить sample request; старый mode не должен оставлять unanswered credit.

**Ограничение проверки.** Реальный USB режим и renegotiation сервера не запускались; C interleaving воспроизведён с fake device и реальными capture modules.

<a id="f13"></a>

**F13. [P2] Ранний AUDIO_INPUT OPEN теряется при первом присоединении session handle**

Участок: Capture / RDPEAI lifecycle. Тип: ошибка. Уверенность: высокая.

Места: [native/src/capture_slots.c:94](/home/truebest/psrc/gnomecast/native/src/capture_slots.c:94), [native/src/capture_slots.c:165](/home/truebest/psrc/gnomecast/native/src/capture_slots.c:165), [native/src/native_session.c:237](/home/truebest/psrc/gnomecast/native/src/native_session.c:237), [native/src/native_session.c:243](/home/truebest/psrc/gnomecast/native/src/native_session.c:243), [webrdp-min/src/native/ffi.rs:147](/home/truebest/psrc/gnomecast/webrdp-min/src/native/ffi.rs:147), [webrdp-min/src/native/rdpeai.rs:182](/home/truebest/psrc/gnomecast/webrdp-min/src/native/rdpeai.rs:182).

**Сценарий и последствия.** Rust worker запускается до возврата rdp_session_start. Если launching/SDL thread задержан, сервер успевает открыть AUDIO_INPUT и прислать OPEN; C callback записывает negotiated stream в слот без handle. Затем NULL→session set_slot безусловно очищает audio_streaming/frame size. Сервер получил успешный OPEN и повторять его не обязан; microphone worker не видит desired stream и молчит до нового OPEN/reconnect.

**Проверка.** Проверены реальный spawn-before-return и start-before-register callers. /tmp/lgnome-capture-early-audio-repro.c и CLI boundary_probe.c воспроизводят streaming=0 packets=0 opens=0 при OPEN перед set_slot.

**Минимальное исправление.** Публиковать session/control state до запуска worker либо сохранять корректную early negotiation при первом NULL→handle присоединении, продолжая очищать её при detach/replacement.

**Тест.** Вызвать AUDIO_INPUT OPEN callback перед первым set_slot, затем присоединить разрешённый foreground handle и потребовать PCM/pacing silence без второго OPEN.

**Ограничение проверки.** Редкое scheduling interleaving инъецировано детерминированно; полный сетевой handshake с остановкой SDL thread не воспроизводился.

<a id="f14"></a>

**F14. [P2] Отказ занятого camera mailbox безвозвратно потребляет новый sample credit**

Участок: Capture / camera credits and mailbox. Тип: ошибка. Уверенность: высокая.

Места: [native/src/capture_slots.c:23](/home/truebest/psrc/gnomecast/native/src/capture_slots.c:23), [native/src/capture_slots.c:28](/home/truebest/psrc/gnomecast/native/src/capture_slots.c:28), [native/src/capture_camera_worker.c:176](/home/truebest/psrc/gnomecast/native/src/capture_camera_worker.c:176), [webrdp-min/src/native/media_mailbox.rs:121](/home/truebest/psrc/gnomecast/webrdp-min/src/native/media_mailbox.rs:121), [webrdp-min/src/native/media_mailbox.rs:137](/home/truebest/psrc/gnomecast/webrdp-min/src/native/media_mailbox.rs:137), [webrdp-min/src/native/rdpecam/bridge.rs:310](/home/truebest/psrc/gnomecast/webrdp-min/src/native/rdpecam/bridge.rs:310).

**Сценарий и последствия.** Старый AU лежит в однопозиционном mailbox. В одной входящей партии сервер делает StopStreams→StartStreams→SampleRequest. Новый credit приходит до drain старого AU; ни новый AU, ни fallback SampleError не помещаются, но C helper всё равно обнуляет credit. Старый AU позже отвергается по generation. Новый Rust request остаётся pending с credit_notified=true, callback повторно не приходит; камера застревает до очередного lifecycle restart.

**Проверка.** CLI и ручной trace stop/start generations, mailbox и active drain/receive loops. Реальные CameraBridge+MediaGate в /tmp/lgnome-capture-audit-20261010/mailbox_probe.rs показывают new AU enqueue=false, error enqueue=false, после stale drain notifications не растут. boundary_probe.c показывает C credit=0 и отсутствие retry ответа.

**Минимальное исправление.** Сохранять и повторять credit/error до успешного enqueue ответа, либо retire прежние generation submissions до выдачи нового credit. Не считать отказанный SampleError завершённым ответом.

**Тест.** Занять mailbox старым AU, выполнить stop/start/new request, отказать обоим enqueue, освободить mailbox и проверить, что новый credit получает AU или SampleError.

**Ограничение проверки.** Протокольная последовательность и обе границы воспроизведены отдельными детерминированными probes; полноценный server+USB integration test не запускался.

<a id="f15"></a>

**F15. [P2] ALSA suspend recovery может навсегда задержать сохранение settings или shutdown**

Участок: Capture / ALSA lifecycle. Тип: риск. Уверенность: высокая.

Места: [native/src/audio_input_alsa.c:314](/home/truebest/psrc/gnomecast/native/src/audio_input_alsa.c:314), [native/src/capture_audio_worker.c:210](/home/truebest/psrc/gnomecast/native/src/capture_audio_worker.c:210), [native/src/capture_redirect.c:145](/home/truebest/psrc/gnomecast/native/src/capture_redirect.c:145), [native/src/capture_redirect.c:193](/home/truebest/psrc/gnomecast/native/src/capture_redirect.c:193).

**Сценарий и последствия.** USB microphone возвращает -ESTRPIPE; snd_pcm_resume продолжает возвращать -EAGAIN, пока устройство suspended. snd_pcm_recover делает неограниченные повторы с 1s sleep/poll даже для NONBLOCK PCM. Audio worker не проверяет stop внутри вызова; HUB Save/reconfigure и shutdown синхронно ждут pthread_join и могут зависнуть.

**Проверка.** CLI независимо подтвердил сценарий. Disassembly установленной /usr/lib/x86_64-linux-gnu/libasound.so.2 показывает resume(-EAGAIN)→poll(1000)→resume без deadline. Проверены read caller, stop flag и synchronous joins; nonblocking read не ограничивает recovery.

**Минимальное исправление.** Обрабатывать -ESTRPIPE как close/retry через существующий stop-aware outer loop либо реализовать отдельное ограниченное по времени и отменяемое resume recovery.

**Тест.** Injectable ALSA read=-ESTRPIPE/resume=-EAGAIN: read/reconfigure/destroy должны возвращаться за ограниченное время. Текущий audio-input-alsa test не покрывает suspend lifecycle.

**Ограничение проверки.** Подтверждена host ALSA реализация; версия целевой ALSA, физический USB suspend и частота проблемы на TV не проверены.

<a id="f16"></a>

**F16. [P2] Capture close ждёт весь TLS drain без общего deadline и может надолго блокировать UI**

Участок: Capture / transport close barrier. Тип: место для внимания. Уверенность: высокая.

Места: [native/src/capture_slots.c:47](/home/truebest/psrc/gnomecast/native/src/capture_slots.c:47), [webrdp-min/src/native/media_mailbox.rs:105](/home/truebest/psrc/gnomecast/webrdp-min/src/native/media_mailbox.rs:105), [webrdp-min/src/native/active_io.rs:144](/home/truebest/psrc/gnomecast/webrdp-min/src/native/active_io.rs:144), [webrdp-min/src/native/connection.rs:67](/home/truebest/psrc/gnomecast/webrdp-min/src/native/connection.rs:67), [webrdp-min/src/native.rs:321](/home/truebest/psrc/gnomecast/webrdp-min/src/native.rs:321).

**Сценарий и последствия.** Сервер медленно читает разрешённый крупный camera AU. Каждая socket write успевает продвинуться раньше per-syscall 5s timeout, но write_all/rustls flush занимают существенно больше. Synchronous capture close ждёт send permit под C lock, поэтому переключение foreground, capture save и shutdown могут надолго удержать SDL thread. Это не доказательство бесконечного lock-cycle: полностью stopped peer должен получить socket timeout.

**Проверка.** Прослежены C foreground publication, MediaGate condvar, permit lifetime через TLS write/flush и TcpStream timeout. RDP reviewer независимо подтвердил отсутствие lock-cycle и отсутствие общего write deadline. Нет transport-boundary slow-peer теста.

**Минимальное исправление.** Добавить абсолютный deadline и cancellation-aware transport write/flush. При превышении закрывать/обрывать transport, сохраняя запрет payload после privacy close; timeout одной condvar без отмены send этот запрет нарушит.

**Тест.** TLS peer принимает короткие chunks быстрее socket timeout, но total send выходит за общий лимит; проверять bounded handoff и отсутствие старых payload после close.

**Ограничение проверки.** Slow TLS runtime repro и latency на TV не измерены; attention относится к подтверждённому пути ожидания и отсутствующей временной границе.

<a id="f17"></a>

**F17. [P2] Завершение connect и нефокусные SHOWN/RESTORED события перехватывают USB input у TV overlay**

Участок: Runtime / focus and input ownership. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_streaming_transition.c:102](/home/truebest/psrc/gnomecast/native/src/native_streaming_transition.c:102), [native/src/native_streaming_transition.c:106](/home/truebest/psrc/gnomecast/native/src/native_streaming_transition.c:106), [native/src/native_streaming_transition.c:35](/home/truebest/psrc/gnomecast/native/src/native_streaming_transition.c:35), [native/src/native_sdl_events.c:83](/home/truebest/psrc/gnomecast/native/src/native_sdl_events.c:83), [native/src/native_input_route.c:404](/home/truebest/psrc/gnomecast/native/src/native_input_route.c:404), [native/src/native_input_route.c:441](/home/truebest/psrc/gnomecast/native/src/native_input_route.c:441).

**Сценарий и последствия.** Начать connect, затем открыть TV menu/уйти в background до ACTIVE. SDL выставляет unfocused/backgrounded и отпускает EVIOCGRAB. ACTIVE transition стирает unfocused и без проверки запускает grab, хотя foreground focus ещё не вернулся. Дополнительно SHOWN/RESTORED обрабатываются как FOCUS_GAINED и также вызывают re-grab при streaming ACTIVE без проверки keyboard focus. Мышь/клавиатура отбираются у системного UI, возможна отправка ввода remote desktop.

**Проверка.** Реальная transition со stub input в /tmp/lgnome-module-audit/repro.c подтверждает grab и clearing focus flag при backgrounded=true. Реальные native_start_streaming_input/native_resume_streaming_input не проверяют focus/background. SHOWN/RESTORED обозначают показ/восстановление размера, а keyboard focus имеет отдельный FOCUS_GAINED; проверено по официальной документации [SDL2 WindowEventID](https://wiki.libsdl.org/SDL2/SDL_WindowEventID).

**Минимальное исправление.** Сохранить реальный lifecycle state, включать grab/input arming только при foreground keyboard focus. SHOWN/RESTORED не считать подтверждением focus; проверить SDL_GetKeyboardFocus/window flags и app_backgrounded перед re-grab.

**Тест.** Connect→FOCUS_LOST/BACKGROUND→ACTIVE без regain: grab count=0, input inactive. Streaming FOCUS_LOST→SHOWN/RESTORED без FOCUS_GAINED также не re-grab. Подтверждённый regain возвращает grab.

**Ограничение проверки.** Host reproducer проверяет dispatch, не настоящий EVIOCGRAB/firmware. Частота отдельных SHOWN/RESTORED последовательностей на webOS не измерена.

<a id="f18"></a>

**F18. [P2] Смерть выбранного survivor перед handoff оставляет пустой скрытый streaming screen**

Участок: Runtime / terminal session supervisor. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_session_supervisor.c:60](/home/truebest/psrc/gnomecast/native/src/native_session_supervisor.c:60), [native/src/native_session_supervisor.c:61](/home/truebest/psrc/gnomecast/native/src/native_session_supervisor.c:61), [native/src/native_switch_handoff.c:32](/home/truebest/psrc/gnomecast/native/src/native_switch_handoff.c:32).

**Сценарий и последствия.** Смотрим slot0, он падает. Supervisor выбирает ACTIVE slot1, но его worker выставляет terminal/session_failed между выбором и handoff guard. Handoff возвращается без ownership flip; supervisor всё равно останавливает slot0, затем slot1 как background failure. Получается sessions=0, active_index=0, streaming_visible=true, preconnect UI hidden. Последующие ticks UI не восстанавливают.

**Проверка.** Реальные native_session_supervisor_tick и native_complete_session_switch с deterministic wrapper /tmp/lgnome-module-audit/repro.c: terminal выбранного survivor инъецирован прямо перед guard; assertions подтверждают скрытый UI и streaming latch после drain и следующего tick. CLI независимо оформил finding.

**Минимальное исправление.** Подтверждать успешный handoff/живого нового owner; при отказе выбрать другого survivor либо выполнить стандартный no-survivor UI/input cleanup. Возможен финальный ownership reconcile после terminal drain.

**Тест.** Инъецировать terminal выбранного survivor перед handoff guard, затем drain обеих ошибок: ожидать streaming=false, UI visible и освобождённый input.

**Ограничение проверки.** Конкурентный terminal event воспроизведён детерминированно с mocked peripheral teardown; полная сеть двух упавших серверов не запускалась.

<a id="f19"></a>

**F19. [P2] Открытый mixer после H264→RGBA исчезает визуально, но продолжает владеть вводом**

Участок: Runtime / presentation and mixer ownership. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_presentation.c:267](/home/truebest/psrc/gnomecast/native/src/native_presentation.c:267), [native/src/native_presentation.c:213](/home/truebest/psrc/gnomecast/native/src/native_presentation.c:213), [native/src/native_mixer_overlay.c:99](/home/truebest/psrc/gnomecast/native/src/native_mixer_overlay.c:99), [native/src/native_rdp_video.c:116](/home/truebest/psrc/gnomecast/native/src/native_rdp_video.c:116).

**Сценарий и последствия.** Открыть mixer на H264, затем сервер переключается на bitmap после >1500ms без H264. Callback создаёт RGBA, не закрывая уже открытый mixer. Каждый presentation tick рисует RGBA и возвращается до panel rendering/auto-hide. Desktop виден, но mixer_overlay_visible остаётся true: RDP input отключён, mixer shortcuts потребляют keys и idle timeout не помогает. Guard mixer_show защищает только первоначальное открытие поверх уже-RGBA, а не этот переход.

**Проверка.** CLI real presentation со stub SDL в /tmp/lgnome-module-audit/repro.c подтверждает bypass истекшего timer. Реальный поддержанный H264→RGBA переход существует в native/tests/test_native_rdp_video.c:118–157; playback reviewer подтвердил, что callback не закрывает mixer. Проверены input arming и show guard.

**Минимальное исправление.** При переходе на RGBA закрывать уже открытый mixer с корректным завершением pointer ownership/held buttons, либо выполнять его lifecycle до любого presentation early return и поддержать software-background composition.

**Тест.** Show mixer на H264→bitmap transition→idle deadline: панель остаётся корректно видимой либо закрывается и возвращает input; невидимого владельца input быть не должно.

**Ограничение проверки.** NDL/LVGL hardware композиция не запускалась; подтверждены actual callback state transition и actual presentation/lifecycle branch.

<a id="f20"></a>

**F20. [P2] Задержанный suppress tail может вклиниться внутрь snapshot replay и нарушить порядок H264 AU**

Участок: Runtime / snapshot replay ordering. Тип: риск. Уверенность: высокая.

Места: [native/src/native_switch_recovery.c:92](/home/truebest/psrc/gnomecast/native/src/native_switch_recovery.c:92), [native/src/native_switch_recovery.c:94](/home/truebest/psrc/gnomecast/native/src/native_switch_recovery.c:94), [native/src/native_switch_handoff.c:78](/home/truebest/psrc/gnomecast/native/src/native_switch_handoff.c:78), [native/src/native_switch_handoff.c:104](/home/truebest/psrc/gnomecast/native/src/native_switch_handoff.c:104), [native/src/native_rdp_video.c:174](/home/truebest/psrc/gnomecast/native/src/native_rdp_video.c:174).

**Сценарий и последствия.** Cache содержит IDR1+delta2, suppressed поток молчит более500ms. Ранее отправленный tail delta3 задержан в сети или worker scheduling и приходит во время replay после active_index flip. Replay выдаёт IDR1, worker выдаёт live delta3, затем replay older delta2: decoder reference order нарушен, возможны артефакты/NEED_KEYFRAME/reconnect. Условие существенно: late tail приходит после quiet window, а cache содержит несколько AU; 500ms quiet лишь снижает вероятность, но не сериализует callbacks.

**Проверка.** Реальные native_snapshot_replay+native_video_ingest_au с fake decoder и deterministic worker barrier /tmp/lgnome-module-audit/replay_repro.c воспроизводят decoder order1,3,2. Каждый ingest использует отдельный video_lock; barrier всей replay chain отсутствует. CLI подтвердил тот же finding.

**Минимальное исправление.** Устанавливать replay barrier до ownership publication и сериализовать всю cached chain перед live callbacks либо буферизовать late tail до replay completion. Просто отбрасывать tail недостаточно: будущие deltas могут ссылаться на него.

**Тест.** Инъецировать live worker AU между cached seed и следующей cached delta и требовать порядок1,2,3. Дополнительно проверить oversized tail queue fallback без нарушения decoder reference chain.

**Ограничение проверки.** Частота delayed-tail interleaving на TV и реакция NDL decoder не измерены. Подтверждён порядок в реальном C ingest, а не видимые артефакты на аппаратном декодере; поэтому finding отмечен risk.

<a id="f21"></a>

**F21. [P2] Первый PCM-пакет теряется при гонке открытия источника с аудиопомпой**

Участок: Native playback / audio. Тип: ошибка. Уверенность: высокая.

Места: [native/src/audio_pipeline_source.c:347](/home/truebest/psrc/gnomecast/native/src/audio_pipeline_source.c:347), [native/src/audio_pipeline_source.c:474](/home/truebest/psrc/gnomecast/native/src/audio_pipeline_source.c:474).

**Сценарий и последствия.** Помпа уже применила старую generation неоткрытого источника. До проверки open/converter_initialized RDP-поток публикует новый формат и первый PCM-пакет. open уже true, converter_initialized ещё false: source_mark_unavailable устанавливает read_cursor в новый write_cursor и удаляет весь опубликованный пакет. Следующая generation не может вернуть read_cursor назад. Короткий звук пропадает целиком; непрерывный поток начинается только со следующего пакета.

**Проверка.** Детерминированное воспроизведение на неизменённых исходниках через существующий before_ring_read hook: /tmp/lgnome-playback-race-repro/repro-public. Очередь после set_format+push =20 ms; после устаревшего unavailable решения =0 ms; после следующего tick =0 ms. CLI независимо воспроизвёл тот же случай со100ms пакетом.

**Минимальное исправление.** Убрать поздний flush read_cursor→write_cursor из source_mark_unavailable; close/format уже публикуют точную границу reset_cursor, которую consumer применяет в source_apply_generation.

**Тест.** Добавить test_before_ring_read для первоначально неоткрытого источника: hook публикует формат и PCM, очередь сохраняется, пакет затем воспроизводится. Дополнить проверкой параллельного producer/pump при close/reopen.

<a id="f22"></a>

**F22. [P2] Background bitmap refinement ошибочно переводит H.264-сессию на немедленное переключение**

Участок: Native playback / video callbacks. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_rdp_video.c:76](/home/truebest/psrc/gnomecast/native/src/native_rdp_video.c:76), [native/src/native_switch_navigation.c:161](/home/truebest/psrc/gnomecast/native/src/native_switch_navigation.c:161), [native/src/native_switch_recovery.c:82](/home/truebest/psrc/gnomecast/native/src/native_switch_recovery.c:82).

**Сценарий и последствия.** Фоновый H.264-поток уже сохранил SPS/PPS+IDR; вслед за ним приходит bitmap refinement. on_bitmap_update безусловно выставляет video_via_bitmap=true и выходит до H.264 hold. Быстрое возвращение в этот слот обходит500ms quiet gate. При suppressed=true native_snapshot_replay забирает и освобождает ещё свежий пригодный cache, после чего требуется refresh либо новый reconnect. При snapshot_pending раннее переключение также сбрасывает cache и ждёт живой seed.

**Проверка.** Повторён CLI host-repro /tmp/lgnome-playback-audit/repro-background-refinement: background H264 seed+refinement =>via_bitmap=1,seed_ready=1,quiet_age=1ms. Foreground аналог сохраняет H.2641500ms. Последствия перепроверены по navigation/handoff/recovery.

**Минимальное исправление.** Учитывать недавние H.264 AUs в состоянии каждого слота и применять refinement hold к background классификации; bitmap refinement не должен отменять H.264 snapshot path.

**Тест.** Background IDR→bitmap refinement→fast return: ready_now остаётся false до quiet gate, seed/cache сохраняется, переключение обходится без лишнего refresh/reconnect. Проверить также настоящий bitmap-only слот.

**Ограничение проверки.** Воспроизведена классификация; переключение и reconnect прослежены по коду, реальные TV артефакты не измерялись.

<a id="f23"></a>

**F23. [P2] UI молча обрезает многобайтовый пароль при Save и Save-and-connect**

Участок: UI / saved profiles. Тип: ошибка. Уверенность: высокая.

Места: [native/src/ui_preconnect_profile.c:50](/home/truebest/psrc/gnomecast/native/src/ui_preconnect_profile.c:50), [native/src/ui_preconnect_forms.c:167](/home/truebest/psrc/gnomecast/native/src/ui_preconnect_forms.c:167), [native/src/ui_preconnect_widgets.c:145](/home/truebest/psrc/gnomecast/native/src/ui_preconnect_widgets.c:145).

**Сценарий и последствия.** LVGL max_length ограничивает число Unicode символов, а NativeSessionConfig.password имеет512 байт. Пароль из256 двухбайтовых символов разрешён UI (меньше511 символов), но snprintf сохраняет511 из512 байт и разрывает последний UTF8 символ. ui_form_valid проверяет только непустые значения; последующая проверка requested_password видит уже обрезанную строку и пропускает её. Сохраняются и отправляются изменённые credentials, вход с верным введённым паролем не удаётся.

**Проверка.** LVGL char_is_accepted считает encoded_length (third_party/lvgl/src/widgets/lv_textarea.c:1013). Повторён /tmp/lgnome-ui-audit-20261010/profile-repro на actual ui_store_form_to_slot:256 codepoints,512 input bytes,511 stored bytes,invalid UTF8. Проверены Save/Connect callers и Rust CStr.to_string_lossy boundary.

**Минимальное исправление.** До копирования и persistence проверять длину UTF8 в байтах для password/username/domain с учётом ёмкости модели и connect request. Показывать понятную ошибку либо запрещать insert сверх байтовой границы; не обрезать credentials молча.

**Тест.** Многобайтовые значения ровно на byte limit и выше него: допустимое сохраняется побайтно, превышение отклоняется, ранее сохранённый профиль не меняется.

<a id="f24"></a>

**F24. [P2] Отключение USB-устройства оставляет на RDP удержанную клавишу или кнопку**

Участок: Input / evdev hotplug. Тип: ошибка. Уверенность: высокая.

Места: [native/src/input_evdev.c:356](/home/truebest/psrc/gnomecast/native/src/input_evdev.c:356), [native/src/input_evdev.c:596](/home/truebest/psrc/gnomecast/native/src/input_evdev.c:596), [native/src/native_input_route.c:345](/home/truebest/psrc/gnomecast/native/src/native_input_route.c:345).

**Сценарий и последствия.** Shift/Ctrl либо mouse DOWN уже отправлен серверу. USB-устройство отключается или libevdev возвращает ENODEV. evdev_remove_device закрывает node и обновляет active flags, но не публикует releases/RESET. Main не вызывает native_flush_held_inputs при hotplug; удалённый desktop остаётся в modifier/drag режиме до другого focus/switch/stop boundary.

**Проверка.** Actual evdev_remove_device harness /tmp/lgnome-ui-repro/unplug: после извлечения Shift DOWN consumer-ом и удаления устройства keyboard_active=0,next_event=NONE,reset_pending=0. Отсутствие последующего UP/RESET воспроизведено; сам RDP forwarding в harness не вызывался, залипание прослежено по callers active flags и flush_held_inputs. [Linux evdev_read](https://github.com/torvalds/linux/blob/master/drivers/input/evdev.c) возвращает ENODEV до выдачи событий после пометки node dead, поэтому kernel-synthesized releases не гарантируют получение UP.

**Минимальное исправление.** При remove/read failure отметить очередь RESET и удалить неполную историю под input->lock, затем разбудить main. Его существующий RESET обработчик освободит held keys/buttons. Аналогично обработать окончательный poll failure.

**Тест.** Fake libevdev: DOWN→consumer forwarding→ENODEV/POLLHUP; следующая drain выдаёт RESET и сервер получает UP. Проверить keyboard, mouse и наличие второго подключённого устройства.

**Ограничение проверки.** Физическое отключение на TV не проводилось; проверены логика удаления и разрешённый kernel error path.

<a id="f25"></a>

**F25. [P2] Клавиатурный текст обгоняет предыдущий клик и попадает в другое поле**

Участок: UI / SDL event ordering. Тип: ошибка. Уверенность: высокая.

Места: [native/src/ui_preconnect_display.c:281](/home/truebest/psrc/gnomecast/native/src/ui_preconnect_display.c:281), [native/src/ui_preconnect.c:152](/home/truebest/psrc/gnomecast/native/src/ui_preconnect.c:152).

**Сценарий и последствия.** За один UI tick SDL накопил click в новое textarea и затем TEXTINPUT. Keypad callback selective PeepEvents забирает keys/text, оставляя более ранние pointer events в SDL. Keypad зарегистрирован после pointer; LVGL добавляет timers в head и первым обрабатывает keypad. Символы вставляются в прежнее focused поле, затем клик переводит focus в новое. При выборе Password текст может попасть в ранее открытый Username.

**Проверка.** Полное LVGL воспроизведение /tmp/lgnome-ui-repro/order.c: actual lv_timer_handler,textarea,group и ui_preconnect_display callbacks; fake SDL queue содержит mouse DOWN/UP(second field),TEXTINPUT A. Итог first=A,second=empty,focus_is_second=1,remaining=0. Использованы настоящие LVGL timers, software draw вместо SDL GPU.

**Минимальное исправление.** Сохранять общий порядок SDL keyboard/text/pointer событий при передаче LVGL. Простая перестановка регистрации timers недостаточна: она меняет, какой из противоположных порядков нарушается.

**Тест.** Два textarea: click B→type должен изменить B; type A→click B должен сначала изменить A. Проверить накопленные batches после задержки UI.

<a id="f26"></a>

**F26. [P2] После потери focus микшер продолжает drag без нажатой кнопки**

Участок: UI / mixer pointer. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_sdl_events.c:70](/home/truebest/psrc/gnomecast/native/src/native_sdl_events.c:70), [native/src/native_sdl_events.c:94](/home/truebest/psrc/gnomecast/native/src/native_sdl_events.c:94).

**Сценарий и последствия.** Пользователь тянет fader, открывает TV menu и отпускает mouse там. Focus-loss обработчик обнуляет mixer_overlay_pointer_buttons, признавая, что UP может потеряться, но оставляет mixer_overlay_dragging=true. После возврата focus обычное движение мыши с motion.state=0 всё ещё пишет fader, будто LEFT удерживается.

**Проверка.** Повторён actual native_handle_sdl_event harness /tmp/lgnome-ui-audit-20261010/events-repro: FOCUS_LOST→FOCUS_GAINED→MOUSEMOTION buttons=0 =>dragging=1,fader_writes=1.

**Минимальное исправление.** Завершать drag при потере focus/background и не выполнять fader edit на motion без LEFT. Сохранять имеющуюся безопасную обработку pending dismiss.

**Тест.** Начать drag→focus loss→потерянный UP→focus regain→hover: громкость не меняется и dragging=false. Проверить обычный drag и release.

<a id="f27"></a>

**F27. [P2] Сервер может скрыть или переместить локальный курсор поверх микшера и TV menu**

Участок: Input / cursor ownership. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_pointer.c:154](/home/truebest/psrc/gnomecast/native/src/native_pointer.c:154), [native/src/native_pointer.c:178](/home/truebest/psrc/gnomecast/native/src/native_pointer.c:178), [native/src/native_loop.c:137](/home/truebest/psrc/gnomecast/native/src/native_loop.c:137).

**Сценарий и последствия.** Микшер остаётся поверх streaming screen, либо TV overlay забрал focus. RDP worker продолжает публиковать pointer state/position активного слота. Streaming loop безусловно вызывает cursor_tick и drain_pointer_warp; оба игнорируют mixer_overlay_visible/window_unfocused/app_backgrounded. Новый server HIDDEN отменяет default visible arrow микшера; server position переставляет compositor pointer, когда пользователь работает с локальным fader/menu.

**Проверка.** Проверены callbacks native_rdp_callbacks.c:60-89 и actual cursor_apply HIDDEN→platform hide. Повторён actual native_pointer harness /tmp/lgnome-ui-audit-20261010/pointer-repro при mixer_overlay_visible=true:cursor_applies=1,warps=1. Main loop сохраняет streaming_visible=true при mixer/focus loss.

**Минимальное исправление.** Применять server cursor/warp только когда foreground RDP действительно владеет указателем; сохранять cached shape во время локальных overlays и reassert после возвращения. В mailbox не переносить проигнорированный overlay warp на поздний return.

**Тест.** При mixer visible и window focus loss публиковать server HIDDEN/new shape/position: локальный pointer остаётся visible и не warps. После закрытия микшера/возврата focus актуальный server cursor снова применяется.

**Ограничение проверки.** Platform effects проверены по fake SDL и вызовам actual native code; live TV menu не запускалось.

<a id="f28"></a>

**F28. [P2] Старый getVolume ответ отменяет новые изменения MASTER и обращает следующий шаг вниз**

Участок: UI / Luna volume bridge. Тип: ошибка. Уверенность: высокая.

Места: [native/src/luna_volume.c:118](/home/truebest/psrc/gnomecast/native/src/luna_volume.c:118), [native/src/native_mixer_overlay.c:233](/home/truebest/psrc/gnomecast/native/src/native_mixer_overlay.c:233).

**Сценарий и последствия.** getVolume запрос начал выполняться при volume30. Пользователь увеличил MASTER до36; SET ещё в полёте либо pending. GET возвращает старое30 с действительным token и безусловно заменяет optimistic cache. Следующий UP читает30 и отправляет33 вместо39: новый SET понижает уже запрошенную громкость. Во время drag старый GET также может дёрнуть knob назад; остановка новых polls не отменяет in-flight запрос.

**Проверка.** Повторён callback harness /tmp/lgnome-ui-audit-20261010/volume-repro на actual luna_volume_get_reply_cb: pending requested36, old matching GET30→cache30. Следующий target33 рассчитан по реальному caller как cached+3; полный UP/LS2 dispatch в harness не выполнялся. Token check защищает только от superseded GET, не от более позднего пользовательского SET.

**Минимальное исправление.** Связать GET с generation пользовательских SET и игнорировать устаревший cache update; удерживать latest desired volume для относительных шагов, пока SET/pending не завершились. После этого выполнять свежий GET для восстановления авторитетного значения/отказа.

**Тест.** Fake LS2:GET30→SET33→SET36 pending→старый GET30→UP; next desired должен39. Проверить delayed reply во время drag, rejected SET и fresh GET после завершения.

**Ограничение проверки.** Реальное LS2 service timing на TV не измерялось; воспроизведён допустимый порядок reply callback.

<a id="f29"></a>

**F29. [P2] Массив путей обрезает существующие fallback-профили**

Участок: Конфигурация / поиск профилей. Тип: ошибка. Уверенность: высокая.

Места: [native/include/config_paths.h:9](/home/truebest/psrc/gnomecast/native/include/config_paths.h:9), [native/src/native_config_storage.c:159](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:159), [native/src/native_config_storage.c:219](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:219).

**Сценарий и последствия.** При различных SDL pref path и HOME collector добавляет 9 путей без environment overrides, но ёмкость массива равна 8. Последний /tmp fallback не попадает в список. Два различных overrides увеличивают число до 11; тогда отбрасывается также /media/developer/temp. Профили, сохранённые прежней версией в отброшенном месте, не обнаруживаются.

**Проверка.** Подсчитаны все вызовы add_candidate в реальном collector; CLI воспроизвёл count=8 и отсутствие последнего пути с fake SDL/окружением. Возвращаемые false при переполнении игнорируются. Верхняя граница в текущем коде — 11 различных кандидатов.

**Минимальное исправление.** Увеличить ёмкость до фактического максимума и контролировать результат добавления, чтобы новые пути не пропадали молча.

**Тест.** Запустить реальный collector с различными SDL/HOME и обоими overrides; каждый из 11 путей должен сохраниться. Отдельно загрузить профиль, существующий только в последнем fallback.

**Ограничение проверки.** Конкретное окружение TV в этом аудите не опрашивалось; defect проявляется при указанных различных путях.

<a id="f30"></a>

**F30. [P2] Повторное сохранение в cached path обходит проверку безопасности каталога**

Участок: Конфигурация / безопасность сохранения. Тип: риск. Уверенность: высокая.

Места: [native/src/native_config_storage.c:324](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:324), [native/src/native_config_storage.c:124](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:124), [native/src/native_config_storage.c:148](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:148).

**Сценарий и последствия.** После первого Save родительский каталог fallback удалён и в предсказуемом месте создан чужой writable каталог или symlink. Следующий Save сразу использует cached_path. O_NOFOLLOW защищает только последний компонент .tmp; каталог не проверяется. Владелец каталога может заменить временный/final файл и подменить сохранённый профиль.

**Проверка.** Mocked-webOS lifecycle CLI: после подмены parent на foreign-owned 0777 Save вернул success, число security checks=0. Прослежены open и rename. Это нарушение directory trust boundary; само по себе оно не доказывает чтение пароля из файла 0600.

**Минимальное исправление.** Перед повторным Save проверять каталог; для защиты от замены между проверкой и записью выполнять операции относительно открытого проверенного directory fd. При нарушении trust заново выбирать безопасный путь.

**Тест.** После первого Save заменить parent каталогом другого владельца и symlink: повторная запись должна безопасно отказаться/перевыбрать путь, не публикуя файл в чужом каталоге.

**Ограничение проверки.** Атака требует доступа другого локального UID к родительскому каталогу. На TV такая доступность не проверялась.

<a id="f31"></a>

**F31. [P2] Исправление прав legacy-каталога не делает ранее подложенный файл доверенным**

Участок: Конфигурация / безопасность загрузки. Тип: риск. Уверенность: высокая.

Места: [native/src/config_paths.c:142](/home/truebest/psrc/gnomecast/native/src/config_paths.c:142), [native/src/native_config_storage.c:303](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:303), [native/src/native_config_storage.c:40](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:40).

**Сценарий и последствия.** В owned legacy-каталог с group/other write заранее помещён чужой settings.json или symlink. Startup исправляет directory mode до 0700 и затем читает файл обычным fopen, без проверки владельца и типа открытого файла. Чужая конфигурация может направить подключение на другой endpoint; при передаче ему credentials последствия затрагивают аутентификацию.

**Проверка.** CLI lifecycle probe: healed=1, loadOK=1, alteredHost=1, file-owner checks=0. Ручная проверка подтверждает, что chmod не проверяет уже существующие entries. Явные environment overrides имеют отдельную политику доверия и к этой находке не относятся.

**Минимальное исправление.** Для автоматически обнаруженных файлов использовать O_NOFOLLOW и fstat открытого fd: regular file, ожидаемый UID, отсутствие group/other write. Не считать legacy-directory healing подтверждением происхождения файла.

**Тест.** Legacy owned 0777 directory с foreign-owned readable file и symlink: после healing оба должны быть отвергнуты; собственный regular 0600 файл должен загружаться.

**Ограничение проверки.** Требуется предварительная возможность записи в legacy-каталог. Реальная компрометация и передача credentials не выполнялись.

<a id="f32"></a>

**F32. [P2] Повреждённый JSON считается загруженным и скрывает исправный fallback**

Участок: Конфигурация / JSON и fallback. Тип: ошибка. Уверенность: высокая.

Места: [native/src/settings_json.c:312](/home/truebest/psrc/gnomecast/native/src/settings_json.c:312), [native/src/settings_json.c:339](/home/truebest/psrc/gnomecast/native/src/settings_json.c:339), [native/src/native_config_storage.c:259](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:259).

**Сценарий и последствия.** Пустой файл, произвольный текст или незакрытый root, например {"sessions":[], считаются корректными. Load останавливается на таком higher-priority файле и не доходит до исправного lower-priority профиля.

**Проверка.** Повторён /tmp/lgnome-review-json-repro.c на оригинальных parser/settings modules: все четыре malformed cases accepted=1. CLI отдельно проверил реальный mocked persisted load: lower candidate не читался.

**Минимальное исправление.** До применения модели проверять целый JSON document: root object, закрытые структуры, корректные токены и отсутствие trailing garbage. Ошибка должна оставлять settings неизменными и позволять fallback.

**Тест.** Пустой/не-JSON/незакрытый higher-priority файл плюс валидный lower-priority: загрузить lower. Проверить transactional rejection повреждённого explicit config.

<a id="f33"></a>

**F33. [P2] Успешный Save в нижний fallback теряется при следующем запуске**

Участок: Конфигурация / выбор пути Save. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_config_storage.c:278](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:278), [native/src/native_config_storage.c:332](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:332).

**Сценарий и последствия.** Higher-priority settings.json остаётся readable, но его каталог стал read-only. Save выбирает writable lower-priority путь и сообщает success. Следующий startup снова выбирает старый readable higher-priority файл: пользователь получает прежние настройки.

**Проверка.** CLI mocked lifecycle: saved wheelStep=35, Save success=1, после reload wheelStep=34. Load prioritizes readability, Save — writability; не предусмотрено согласование этих решений.

**Минимальное исправление.** Не сообщать successful persistence в файл, который следующая загрузка заведомо проигнорирует. Минимально — отказать Save при таком shadowing и показать причину.

**Тест.** Readable read-only higher candidate и writable lower: либо Save отклоняется, либо новый запуск гарантированно видит именно сохранённую модель.

<a id="f34"></a>

**F34. [P2] Допустимые настройки сохраняются в файл, который loader отвергает**

Участок: Конфигурация / граница размера файла. Тип: ошибка. Уверенность: высокая.

Места: [native/include/native_config.h:14](/home/truebest/psrc/gnomecast/native/include/native_config.h:14), [native/src/native_config_storage.c:52](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:52), [native/src/native_config_storage.c:139](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:139).

**Сценарий и последствия.** Четыре валидных профиля с длинными строками содержат достаточно символов, требующих JSON escaping. Writer не ограничивает output, заменяет старый файл, но loader имеет предел 16 KiB. При следующем запуске новый файл отвергается.

**Проверка.** Повторён /tmp/lgnome-review-size-repro.c: host=192.0.2.1, четыре строки name/username/password/domain по 511 backslashes на слот; модель проходит runtime validation, saved_bytes=17628, loader_limit=16384, reload=0. Все данные синтетические.

**Минимальное исправление.** Согласовать read bound с худшим escaped размером разрешённой модели либо отклонять oversized output до замены предыдущего файла.

**Тест.** Round-trip максимально длинных escaped strings четырёх профилей. Если лимит превышен, сохранить предыдущий файл и вернуть понятную ошибку.

<a id="f35"></a>

**F35. [P2] Unicode escapes декодируются как однобайтовые символы вместо UTF-8**

Участок: Конфигурация / Unicode JSON. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_json.c:99](/home/truebest/psrc/gnomecast/native/src/native_json.c:99), [native/src/native_json.c:102](/home/truebest/psrc/gnomecast/native/src/native_json.c:102), [webrdp-min/src/native/ffi.rs:56](/home/truebest/psrc/gnomecast/webrdp-min/src/native/ffi.rs:56).

**Сценарий и последствия.** {"name":"caf\u00e9"} принимается, но é записывается одним байтом 0xe9; {"name":"\u0410"} отвергается. Для credentials невалидные байты затем меняются Rust CStr::to_string_lossy, и правильный пароль может превратиться в другой.

**Проверка.** CLI in-memory probe проверил байты результата и rejection кириллицы; вручную проверены h0/h1==0 restriction и однобайтовое присваивание. Literal UTF-8 UI поддерживает, поэтому escaped и literal эквиваленты расходятся.

**Минимальное исправление.** Декодировать Unicode code points и surrogate pairs в UTF-8 с проверкой byte capacity; некорректные escapes отклонять.

**Тест.** Literal UTF-8 и эквивалентные escapes для Latin/Cyrillic/astral символов дают одинаковые bytes; проверить malformed surrogate и переполнение буфера.

<a id="f36"></a>

**F36. [P2] Embedded NUL молча обрезает JSON-строку**

Участок: Конфигурация / C-string boundary. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_json.c:102](/home/truebest/psrc/gnomecast/native/src/native_json.c:102), [native/src/native_json.c:113](/home/truebest/psrc/gnomecast/native/src/native_json.c:113).

**Сценарий и последствия.** Строка {"name":"a\u0000b"} принимается, но C-string consumers видят только a. При сохранении suffix исчезает. Тот же parser применяется к endpoint и credentials.

**Проверка.** CLI probe: parser success при U+0000, видимая длина=1 и последующий writer теряет suffix. Модель хранит NUL-terminated строки и не может представить такой input без потери.

**Минимальное исправление.** Отклонять декодированный U+0000 до изменения settings.

**Тест.** Embedded NUL в name/host/password приводит к transactional rejection, старые настройки остаются побайтно неизменными.

<a id="f37"></a>

**F37. [P2] Числовые и boolean значения принимаются по префиксу**

Участок: Конфигурация / JSON scalars. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_json.c:131](/home/truebest/psrc/gnomecast/native/src/native_json.c:131), [native/src/native_json.c:148](/home/truebest/psrc/gnomecast/native/src/native_json.c:148), [native/src/native_json.c:162](/home/truebest/psrc/gnomecast/native/src/native_json.c:162).

**Сценарий и последствия.** fps:6e1, wheelStep:1.5 и audioInputGainDb:1e1 принимаются как 6, 1 и 1. Boolean truejunk считается true. strtoul/strtol end pointer и граница boolean token не проверяются.

**Проверка.** CLI scalar probes и повторный malformed_case с cameraEnabled:trueBROKEN подтвердили acceptance. Ручная проверка всех трёх readers показала общий дефект token termination.

**Минимальное исправление.** Требовать полный допустимый scalar token с корректным terminator. Для integer-полей либо отвергать fractions/exponents, либо явно поддерживать их без чтения только префикса.

**Тест.** Проверить exponent/fraction/trailing suffix, знаки и leading zeros; rejection не должен частично менять настройки.

<a id="f38"></a>

**F38. [P2] Неизвестное поле внутри session переопределяет глобальную настройку**

Участок: Конфигурация / вложенность JSON. Тип: ошибка. Уверенность: высокая.

Места: [native/src/settings_json.c:327](/home/truebest/psrc/gnomecast/native/src/settings_json.c:327), [native/src/native_json.c:28](/home/truebest/psrc/gnomecast/native/src/native_json.c:28).

**Сценарий и последствия.** {"sessions":[{"slot":"red","wheelStep":1}],"wheelStep":60} устанавливает global wheelStep=1. Общий substring scanner находит nested field раньше настоящего top-level. Даже ignored unknown-slot entries могут влиять на globals.

**Проверка.** CLI проба возвращает 1 вместо 60. apply_global_json получает полный документ, native_json_find_value не учитывает nesting/object boundaries.

**Минимальное исправление.** Ограничить global lookup полями outer object; session lookup — соответствующим объектом.

**Тест.** Nested unknown fields перед настоящими globals не влияют на результат; проверить ignored slot и field name внутри string.

<a id="f39"></a>

**F39. [P2] Успешный Save не синхронизирует файл и каталог на диск**

Участок: Конфигурация / долговечность сохранения. Тип: место для внимания. Уверенность: средняя.

Места: [native/src/native_config_storage.c:139](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:139), [native/src/native_config_storage.c:148](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:148).

**Сценарий и последствия.** Питание отключается сразу после Save. fclose и rename сами по себе не устанавливают durability: новые bytes или directory entry могут не попасть на persistent storage.

**Проверка.** В write path отсутствуют fsync/fdatasync файла и fsync parent directory. Реальная потеря данных при отключении питания не воспроизводилась; это место требует решения по контракту сохранения.

**Минимальное исправление.** Перед rename fflush и fsync temporary file, после rename fsync directory; учитывать ошибки, не заявляя гарантированную persistence до завершения.

**Тест.** Fault injection failures flush/fsync/rename: корректная ошибка и сохранность старого файла до replacement. Отдельно проверить durability на целевой filesystem.

**Ограничение проверки.** Power-loss test и filesystem semantics конкретного TV не проверялись.

<a id="f40"></a>

**F40. [P2] Host tests не выполняют production lifecycle persisted settings**

Участок: Конфигурация / тестовое покрытие. Тип: место для внимания. Уверенность: высокая.

Места: [native/CMakeLists.txt:467](/home/truebest/psrc/gnomecast/native/CMakeLists.txt:467), [native/src/native_config_storage.c:340](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:340), [native/src/native_config_storage.c:346](/home/truebest/psrc/gnomecast/native/src/native_config_storage.c:346).

**Сценарий и последствия.** test-settings-json компилирует storage без LGNOME_TARGET_WEBOS: public persisted load/save становятся success no-op. Tests helpers/parser проходят, хотя production collector, load ordering, cached save и directory trust содержат воспроизведённые выше дефекты.

**Проверка.** Проверены CMake target definitions и #ifdef ветки. Обычные CTests прошли; реальные persisted branches понадобилось отдельно включить во временном fake-SDL CLI harness.

**Минимальное исправление.** Добавить один host integration target с реальным webOS storage branch, fake SDL pref path и контролируемой временной filesystem.

**Тест.** Collector capacity, invalid-file fallback, readable read-only shadowing, cache invalidation и legacy trust проверять через public load/save, а не заменять их stubs.

<a id="f41"></a>

**F41. [P2] Обе CI-конфигурации исключают auth и crypto тесты**

Участок: Tooling / CI. Тип: место для внимания. Уверенность: высокая.

Места: [.github/workflows/ci.yml:37](/home/truebest/psrc/gnomecast/.github/workflows/ci.yml:37), [bitbucket-pipelines.yml:26](/home/truebest/psrc/gnomecast/bitbucket-pipelines.yml:26), [docs/native-runbook.md:393](/home/truebest/psrc/gnomecast/docs/native-runbook.md:393).

**Сценарий и последствия.** Регрессия в credssp, ntlm, crypto или der может пройти обязательный Host tests check: cargo test фильтруется строкой native::, а эти suites находятся вне native.

**Проверка.** Текущий test executable перечисляет 87 tests, из них 68 native::. Фильтр исключает 19 тестов: credssp4, crypto6, der4, ntlm5. Общий полный Rust run выполнял root, а CI-команды по-прежнему используют фильтр.

**Минимальное исправление.** Удалить positional filter native:: в обеих CI-командах и соответствующей локальной инструкции.

**Тест.** Проверка test inventory/CI команды должна включать все четыре auth/crypto suites вместе с native tests.

<a id="f42"></a>

**F42. [P2] Requested release version не сверяется с appinfo**

Участок: Tooling / release metadata. Тип: место для внимания. Уверенность: высокая.

Места: [tools/release-github.sh:19](/home/truebest/psrc/gnomecast/tools/release-github.sh:19), [tools/release-github.sh:71](/home/truebest/psrc/gnomecast/tools/release-github.sh:71), [tools/release-github.sh:77](/home/truebest/psrc/gnomecast/tools/release-github.sh:77).

**Сценарий и последствия.** В main есть новые изменения, но appinfo version забыли увеличить. Переданная новая версия всё равно становится commit/tag, а пакет содержит старую версию. Скрипт печатает имя нового IPK, которого обычная сборка этого tree не создаёт.

**Проверка.** Argument version используется для tag, commit message и asset filename; main tree экспортируется без сравнения с main:native/deploy/webos/appinfo.json. Ни до tag, ни до push проверки metadata нет.

**Минимальное исправление.** До создания release objects сравнивать requested version с appinfo экспортируемого main tree; перед публикацией asset проверять его metadata.

**Тест.** Version mismatch прерывает подготовку до tag/push; совпадающие tag, appinfo и IPK version принимаются.

**Ограничение проверки.** Полный release script с фиктивными remotes не запускался; подтверждён source trace.

<a id="f43"></a>

**F43. [P2] Ошибка открытия log path закрывает stderr и теряет все логи**

Участок: Logging / lifecycle. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_lifecycle.c:47](/home/truebest/psrc/gnomecast/native/src/native_lifecycle.c:47).

**Сценарий и последствия.** LGNOME_NATIVE_LOG_PATH указывает на недоступный каталог либо default log не удаётся открыть. freopen закрывает исходный stderr, затем возвращает ошибку; startup продолжается, последующие clog и Rust callback diagnostics исчезают.

**Проверка.** Probe с оригинальными native_lifecycle.c/category_log.c и синтетическим отсутствующим path подтвердил fcntl(2,F_GETFD)==-1 после native_prepare_webos_logging.

**Минимальное исправление.** Сначала открыть destination, затем перенаправить stderr только при успехе; при open failure сохранить старый stderr и вывести диагностическое сообщение.

**Тест.** Недоступный path оставляет fd2 валидным и сообщения доступными; успешный path сохраняет ожидаемую redirection.

<a id="f44"></a>

**F44. [P2] IPK теряет sticky bit settings directory**

Участок: Tooling / packaging. Тип: риск. Уверенность: высокая.

Места: [tools/build-native-webos.sh:255](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:255), [tools/build-native-webos.sh:257](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:257), [tools/build-native-webos.sh:282](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:282).

**Сценарий и последствия.** Staging создаёт settings/ с mode1777, но pinned ares-package пересоздаёт directory и кладёт в payload0777. Другой локальный UID, имеющий доступ к application tree, может переименовать/убрать чужой private euid directory вопреки его mode0700; persisted profiles перестают быть доступны.

**Проверка.** На свежем root cross-build artifact /tmp/lgnome-modular-review-webos-dist/com.truebest.lgnome.native_0.5.5_arm.ipk tar metadata settings mode=0777, тогда как staging mode=1777. CLI package.js:866 mkdirp не сохраняет directory mode. verify_package_root/verify_ipk mode не проверяют и принимают payload.

**Минимальное исправление.** Сохранять mode1777 при формировании data.tar payload и проверять sticky bit в финальном IPK, отклоняя неверные права.

**Тест.** Новый пакет сохраняет settings1777; verifier отвергает0777 payload; изолированный different-UID сценарий не позволяет переименовать private subdir.

**Ограничение проверки.** Доступ других TV UID к approot и возможность эксплуатации в webOS jail не проверялись; речь об условном local threat и достоверно неверном payload mode.

<a id="f45"></a>

**F45. [P2] Cargo target directory override приводит к старому или отсутствующему Rust staticlib**

Участок: Tooling / cross-build. Тип: место для внимания. Уверенность: высокая.

Места: [tools/build-native-webos.sh:207](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:207), [tools/build-native-webos.sh:231](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:231), [tools/build-native-webos.sh:232](/home/truebest/psrc/gnomecast/tools/build-native-webos.sh:232).

**Сценарий и последствия.** Пользователь задаёт CARGO_TARGET_DIR либо Cargo build.target-dir, без ручного RDP_FFI_LIB override. Cargo собирает в выбранный directory, но script проверяет старый webrdp-min/target path: чистая сборка ошибочно завершается, а если там есть прежний archive, CMake может связать его и упаковать устаревший Rust code.

**Проверка.** Cargo metadata с CARGO_TARGET_DIR=/tmp/lgnome-review-cargo-output подтвердил target_directory override. cargo build вызывается без --target-dir, а rdp_ffi_lib заранее вычисляется через hardcoded default. Существование файла не подтверждает его свежесть.

**Минимальное исправление.** Задать явный совпадающий --target-dir либо вывести staticlib path из фактического Cargo target_directory; сохранить explicit RDP_FFI_LIB override.

**Тест.** Сборка с другим CARGO_TARGET_DIR и stale archive в default path использует только свежий artifact; clean override также успешно разрешается.

**Ограничение проверки.** Повторный cross-build ради override не выполнялся; подтверждены Cargo metadata и connected source paths.

**Приоритет P3**

<a id="f46"></a>

**F46. [P3] Параллельное чтение queue_ms может показывать десятки миллионов миллисекунд**

Участок: Native playback / statistics. Тип: ошибка. Уверенность: высокая.

Места: [native/src/audio_pipeline_source.c:31](/home/truebest/psrc/gnomecast/native/src/audio_pipeline_source.c:31), [native/src/audio_pipeline_source.c:750](/home/truebest/psrc/gnomecast/native/src/audio_pipeline_source.c:750).

**Сценарий и последствия.** SDL observer читает write_cursor, затем producer добавляет PCM, а pump успевает прочитать новые кадры. Observer получает read_cursor больше сохранённого write_cursor; unsigned write-read переполняется. Mixer временно показывает невозможную задержку.

**Проверка.** Повторён детерминированный CLI repro /tmp/lgnome-playback-audit/repro-queue-observer: первый stats.queue_ms=89478475, следующая согласованная выборка=90. Подмена atomic-load во временном harness моделирует разрешённое межпоточное чередование.

**Минимальное исправление.** Для observer статистики получать согласованную или проверенную пару cursors; повторять выборку, если unsigned distance превышает capacity. Не сообщать такое переполнение как реальную очередь.

**Тест.** Между двумя observer loads сдвинуть producer и consumer и проверить0≤queued_frames≤ring_capacity.

**Ограничение проверки.** Подтверждён дефект advisory telemetry; повреждение PCM или jitter controller этим воспроизведением не показано.

<a id="f47"></a>

**F47. [P3] Fallback ввода игнорирует все цифры цифрового блока**

Участок: UI / keyboard fallback. Тип: ошибка. Уверенность: высокая.

Места: [native/src/ui_preconnect_display.c:148](/home/truebest/psrc/gnomecast/native/src/ui_preconnect_display.c:148).

**Сценарий и последствия.** Когда SDL_TEXTINPUT отсутствует, text_key_from_sdl должен перевести numeric keypad keydown в цифру. Условие sym>=SDLK_KP_0&&sym<=SDLK_KP_9 невыполнимо: SDL scancode KP_0=98,а KP_1..KP_9=89..97. Поэтому ввод адреса/порта/password с цифрового блока пропадает.

**Проверка.** Повторён /tmp/lgnome-ui-audit-20261010/display-repro:KEYDOWN KP_1+KMOD_NUM возвращает supported=0. Численные константы проверены в установленном target SDL header.

**Минимальное исправление.** Отдельно обработать KP_0, затем непрерывный диапазон KP_1..KP_9 с базой1; учесть NumLock semantics выбранного fallback.

**Тест.** Все десять KP digits без SDL_TEXTINPUT, top-row digits и наличие TEXTINPUT, чтобы исключить дубли.

<a id="f48"></a>

**F48. [P3] Координаты server warp могут собраться из разных публикаций**

Участок: Input / pointer mailbox. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_rdp_callbacks.c:86](/home/truebest/psrc/gnomecast/native/src/native_rdp_callbacks.c:86), [native/src/native_pointer.c:155](/home/truebest/psrc/gnomecast/native/src/native_pointer.c:155), [native/src/native_pointer.c:165](/home/truebest/psrc/gnomecast/native/src/native_pointer.c:165).

**Сценарий и последствия.** В mailbox уже pending(100,100). Producer начинает следующий update(500,500) и успевает записать x=500. SDL очищает старый pending и читает x/y до записи нового y; вызывает Warp(500,100), хотя сервер такую точку не присылал. При совпадении с переключением owner отдельно читаемый slot также не является согласованным payload.

**Проверка.** Повторён /tmp/lgnome-ui-audit-20261010/pointer-repro на actual native_drain_pointer_warp:старый(100,100),частичный новый(500,500)→реальный warp(500,100). Чередование допустимо даже с sequentially consistent atomics.

**Минимальное исправление.** Публиковать/забирать x,y,slot и pending как единый snapshot под одной синхронизацией, с проверкой owner внутри неё. Отдельные atomic fields и pending flag не обеспечивают согласованность payload.

**Тест.** Детерминированно остановить producer между x и y при уже pending update; consumer должен получить целиком старый либо новый tuple. Дополнить owner change case.

**Ограничение проверки.** Показан ограниченный transient pointer jump; длительный TV-visible эффект не измерялся.

<a id="f49"></a>

**F49. [P3] CLI молча игнорирует опцию без обязательного значения**

Участок: Конфигурация / CLI. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_config_launch.c:176](/home/truebest/psrc/gnomecast/native/src/native_config_launch.c:176), [native/src/native_config_launch.c:180](/home/truebest/psrc/gnomecast/native/src/native_config_launch.c:180).

**Сценарий и последствия.** Терминальный --fps игнорируется, а bare --config считается отсутствующим. Пользователь явно запросил config, но приложение выбирает default/persisted settings вместо ошибки синтаксиса.

**Проверка.** CLI compiled probe: missing --fps success=1,fps=60; bare --config detected_as_explicit=0,falls_back_to_default=1. Проверен main path выбора файла.

**Минимальное исправление.** Различать absent option и present-without-value; для известных опций отклонять отсутствие значения при launch validation.

**Тест.** Bare --fps/--config и значение, заменённое следующей опцией: ожидается diagnostic/error; отсутствующая опция сохраняет обычные defaults.

<a id="f50"></a>

**F50. [P3] Host startup проверяет green даже при --connect-slot red**

Участок: Конфигурация / host startup. Тип: ошибка. Уверенность: высокая.

Места: [native/src/native_config.c:120](/home/truebest/psrc/gnomecast/native/src/native_config.c:120), [native/src/main.c:221](/home/truebest/psrc/gnomecast/native/src/main.c:221).

**Сценарий и последствия.** В non-webOS сборке настроен только red и выбран --connect-slot red. native_config_validate требует green credentials и останавливает startup раньше, чем main выбирает red.

**Проверка.** Compiled CLI probe: requested=red,chosen_slot_ready=1,startup_config_ready=0. Ручная проверка main подтвердила отдельный selected-slot startup после общей validation.

**Минимальное исправление.** Определять startup slot до readiness validation и проверять выбранный профиль вместе с global runtime constraints.

**Тест.** Host smoke launch с готовым red и пустым green должен дойти до red connection; invalid выбранный профиль отклоняется.

**Ограничение проверки.** Ограничено non-webOS startup/smoke path; webOS preconnect UI использует relaxed validation.

**Выполненные проверки исходного аудита**

| Проверка | Результат |
| --- | --- |
| `./tools/syntax-check-native.sh` | PASS: 68 файлов; дополнительные policy checks: 78 C и 28 Rust. Host syntax-check исключает часть SDL/LVGL/evdev; они скомпилированы cross-build ниже. |
| `cargo fmt --manifest-path webrdp-min/Cargo.toml --check` | PASS. |
| `cargo test --manifest-path webrdp-min/Cargo.toml --features native --locked native::` | 68/68 PASS. |
| `cargo test --manifest-path webrdp-min/Cargo.toml --features native --locked` | 87/87 PASS; дополнительно выполнены 19 auth/crypto/DER tests, отсутствующих в CI filter. |
| Host CMake build и `ctest --output-on-failure` | 74/74 PASS, включая backend_ndl и deploy-webos. |
| `python3 tools/test-deploy-native-webos.py` | PASS. Текущий recorder test не проверяет shell reserialization внутри настоящего ares CLI; см. находку о deployment. |
| Полный `./tools/build-native-webos.sh` в отдельных `/tmp` build/dist dirs | PASS: Rust ARMv7, native C, SDL/LVGL/evdev и Opus, проверка staging и распакованного IPK. |
| Scoped UI webOS syntax-check, LVGL/event harnesses, security/lifecycle/media/FS probes | Пройдены; конкретные результаты приведены в соответствующих находках. Это детерминированные harnesses с actual modules и необходимыми stubs, не live TV integration. |

Команды host-сборки:

```sh
cmake -S native -B /tmp/lgnome-modular-review-20261010-build \
  -DCMAKE_PREFIX_PATH=/home/truebest/.cache/lgnome-review-fixes/status-googletest
cmake --build /tmp/lgnome-modular-review-20261010-build -j6
ctest --test-dir /tmp/lgnome-modular-review-20261010-build --output-on-failure
```

Первый configure не нашёл GTest. Использована уже имеющаяся локальная установка; зависимости и файлы проекта не менялись.

Команда production cross-build:

```sh
NATIVE_WEBOS_BUILD_DIR=/tmp/lgnome-modular-review-webos-build \
NATIVE_WEBOS_DIST_DIR=/tmp/lgnome-modular-review-webos-dist \
  ./tools/build-native-webos.sh
```

Получен проверенный [com.truebest.lgnome.native_0.5.5_arm.ipk](/tmp/lgnome-modular-review-webos-dist/com.truebest.lgnome.native_0.5.5_arm.ipk). Product metadata: `com.truebest.lgnome.native`, версия `0.5.5`, executable `bin/lgnome-native`. Это локальный build artifact; публикация и установка не выполнялись.

При упаковке ares-package сообщил `TypeError: rimraf is not a function` и exit 255 после записи IPK. Существующий build script обработал этот известный случай и успешно проверил созданный IPK. Эта проверка не обнаруживает неверный sticky bit, описанный в отчёте.

Логи общих проверок: [host build](/tmp/lgnome-modular-review-build.log), [CTest](/tmp/lgnome-modular-review-ctest.log), [webOS build/IPK](/tmp/lgnome-modular-review-webos.log).

**Что осталось непроверенным**

Физический TV не подключался: не выполнялись install/launch, EVIOCGRAB/USB hotplug/suspend, реальные LS2 replies, NDL decoding и композиция overlays. Не проводились полная RDP MITM, живой Windows compatibility matrix, slow/stalled-peer integration, намеренный OOM, выполнение oversized-canvas на ARM32 или power-loss test.

Официальный target-NDK header для дополнительного ABI conformance test отсутствовал; базовые backend_ndl tests прошли. В host CMake Opus выключен по умолчанию; Opus включён и собран в production cross-build. Санитайзеры и длительные load/soak tests не запускались.

Подмодули проверены в местах, связанных с продуктом и найденными проблемами; это не отдельный exhaustive audit всего upstream IronRDP/LVGL/miniaudio. Генерированные font blobs не проверялись вручную. Успешные существующие tests не опровергают изложенные interleavings и пробелы покрытия.

На этапе исходного аудита исходники, настройки, CI и release/deploy scripts не исправлялись. Последующая проверка и внесённые исправления описаны в начале этого файла. Условные риски не утверждают аппаратного воспроизведения.
