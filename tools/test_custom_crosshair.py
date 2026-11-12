"""Private native crosshair acceptance through real SDL/ImGui pointer input.

Settings on/off is an F12 fixture; every feature action uses measured, clipped
ImGui widget geometry. Gameplay starts through the original native menus. No
physical desktop input, direct settings mutation or forced map load is used.
The bounded cases cover GPU16:9 UI/persistence/fallback, software gameplay and
two-process local settings, not a campaign playthrough. host-client-direct uses
declared CLI/automatic-intro setup and does not claim original-menu/LAN coverage.
"""
from test_native_mouse import Pointer, COMMAND_FILE, ident, public_menu, select_original_profile
from test_native_mouse_multiplayer import MultiplayerMenus, host_create, client_join_and_roles, begin_campaign, leave_sessions, close_initial_information_by_mouse
from test_coop_campaign import sha
from pathlib import Path
import argparse, hashlib, json, math, os, shutil, subprocess, time, traceback

PNG_NAME = 'Dolphin-Punkt.png'
UI_STATE = 'crosshair-ui-state.json'
RENDER_STATE = 'crosshair-render-state.json'
NATIVE_STATE = 'native-crosshair-state.json'


def record(pointer, action, **values):
    pointer.report['steps'].append({'action': action, **values})
    print(action, flush=True)


def data_stamp(folder):
    # The shared owned game-data folder is only passed as --data, never copied
    # over or written by the driver. Inventory detects unexpected mutation.
    digest = hashlib.sha256()
    for path in sorted(folder.rglob('*')):
        if path.is_file():
            value = path.stat()
            digest.update((path.relative_to(folder).as_posix() + ':' + str(value.st_size) + ':' + str(value.st_mtime_ns) + '\n').encode('utf8'))
    return digest.hexdigest()


def source_state(a):
    return {'exe': sha(a.exe), 'card': sha(a.card), 'png': sha(a.png),
            'dol': sha(a.data / 'sys/main.dol'), 'data_inventory': data_stamp(a.data),
            'settings': {name: sha(a.exe.parent / name) if (a.exe.parent / name).exists() else None
                         for name in ('settings.ini', 'settings-netplay.ini')}}


class CrosshairMenus(MultiplayerMenus):
    def __init__(self, *args):
        super().__init__(*args)
        self.ui_cache = {}; self.render_cache = {}; self.crosshair_cache = {}
        self.launches = []; self.generations = {}

    def log(self, role):
        name = ('ssne-coop-host.log' if role == 'host' else 'ssne-coop-client.log') if self.a.case == 'host-client-direct' else 'ssne.log'
        path = self.folder / role / name
        return path.read_text(encoding='utf8', errors='replace') if path.exists() else ''

    def ui(self, role='host'):
        return self.copied_json(role, UI_STATE, self.ui_cache)

    def render(self, role='host'):
        return self.copied_json(role, RENDER_STATE, self.render_cache)

    def crosshair(self, role='host'):
        return self.copied_json(role, NATIVE_STATE, self.crosshair_cache)

    def diagnostics(self, role='host'):
        return {'ui': self.ui(role), 'render': self.render(role),
                'native_crosshair': self.crosshair(role), 'menu': public_menu(self.state(role)),
                'mouse': self.mouse(role), 'ack': self.mouse_ack(role)}

    def stop_role(self, role):
        child = self.processes[role]
        if child.poll() is None:
            self.pulse('QUIT', (role,)); child.wait(timeout=10)
        assert child.returncode == 0, ('Private game did not exit cleanly', role, child.returncode)
        for entry in self.launches:
            if entry['process'] is child: entry['stream'].close()

    def start_role(self, role, language):
        folder = self.folder / role
        fresh = not folder.exists()
        if fresh:
            folder.mkdir(parents=True); (folder / 'steps').mkdir()
            shutil.copy2(self.a.exe, folder / 'game.exe')
            shutil.copy2(self.a.card, folder / 'card.raw')
            (folder / 'settings.ini').write_text(self.a.private_settings, encoding='utf8')
            if self.a.case == 'host-client-direct':
                (folder / 'settings-netplay.ini').write_text(self.a.private_settings, encoding='utf8')
            (folder / 'crosshairs').mkdir()
            shutil.copy2(self.a.png, folder / 'crosshairs' / PNG_NAME)
        else:
            assert self.processes[role].poll() == 0, 'Restart requires a cleanly stopped role'
            archive = folder / ('previous-run-' + str(self.generations[role]))
            archive.mkdir()
            for name in ('console.log', 'ssne.log', 'performance.jsonl', UI_STATE, RENDER_STATE, NATIVE_STATE,
                         'native-menu-state.json', 'native-menu-mouse-state.json', 'native-mouse-input-state.json'):
                path = folder / name
                if path.exists(): path.rename(archive / name)
            for cache in (self.last_state, self.mouse_cache, self.ack_cache, self.carousel_cache,
                          self.lobby_cache, self.campaign_cache, self.information_cache,
                          self.ui_cache, self.render_cache, self.crosshair_cache):
                cache.pop(role, None)
        self.generations[role] = self.generations.get(role, 0) + 1
        for name in ('input.txt', 'command.txt', COMMAND_FILE):
            (folder / name).write_text('', encoding='ascii')
        env = {key: value for key, value in os.environ.items() if not key.startswith('SSNE_')}
        env.update(SSNE_TEST_HIDDEN='1', SSNE_DEV_NATIVE_MENU='1', SSNE_TEST_DEVICE_INPUT='1',
                   SSNE_TEST_CROSSHAIR='1', SSNE_PAD_SCRIPT_FILE='input.txt',
                   SSNE_NATIVE_MOUSE_COMMAND=COMMAND_FILE, SSNE_NATIVE_MENU_COMMAND='command.txt',
                   SSNE_TEST_SCREENSHOT_DIR='steps', SSNE_PERF_LOG='performance.jsonl',
                   SSNE_NATIVE_MENU_PORT=str(self.a.port), SSNE_NATIVE_MENU_DISCOVERY_PORT=str(self.a.port + 1),
                   SSNE_NATIVE_TEST_LANGUAGE=str(language))
        if role == 'host':
            env.update(SSNE_COOP_CAMPAIGN_VERIFY='1', SSNE_COOP_CAMPAIGN_COMMAND='campaign-command.txt')
        direct = self.a.case == 'host-client-direct'
        if direct:
            env.update(SSNE_COOP_TEST_SKIP_DELAY_MS='1500', SSNE_TEST_SKIP_FIRST_VOTE='1')
        else:
            assert not any(key in env for key in ('SSNE_COOP_TEST_SKIP_DELAY_MS', 'SSNE_TEST_SKIP_FIRST_VOTE'))
        startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
        stream = (folder / 'console.log').open('wb'); self.streams.append(stream)
        argv = [str(folder / 'game.exe'), '--data', str(self.a.data), '--memcard',
                str(folder / 'card.raw'), '--mute', '--fps', str(self.a.fps)]
        if direct:
            argv += ['--lan-port', str(self.a.port), '--lan-pin', '123456', '--player-name', role]
            argv += ['--coop-host', '--coop-capacity', '8', '--coop-level', 'Rlevel1_1'] if role == 'host' else ['--coop-join', '127.0.0.1']
        child = subprocess.Popen(argv, cwd=folder, env=env, stdout=stream, stderr=stream, startupinfo=startup,
                                 creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        self.processes[role] = child
        self.launches.append({'role': role, 'generation': self.generations[role], 'process': child, 'stream': stream})
        if direct:
            self.delay(2 if role == 'host' else .2)
            return  # Declared CLI fixture boots both peers before waiting for readiness.
        deadline = time.monotonic() + 75
        while self.state(role).get('scene') != 3:
            if time.monotonic() > deadline: raise TimeoutError('Original boot ' + str(self.diagnostics(role)))
            self.delay(1)
            if child.poll() is not None: raise RuntimeError('Private game exited during boot ' + str(child.returncode))
            if self.state(role).get('scene') == 2: self.press(role, 'B')  # Boot fixture only.
        self.wait(lambda: self.mouse(role).get('surfaces') and self.mouse_ack(role).get('image_rect')
                  and self.ui(role) and self.render(role), 25)
        self.delay(.5)


class CrosshairPointer(Pointer):
    def button(self, down, uv, role='host', button='left'):
        before = dict(self.ack(role))
        presentation = (down and button == 'left' and before.get('presentation')
                        and not self.observe(role).get('has_surface') and not self.m.ui(role).get('menu_open'))
        result = super().button(down, uv, role, button)
        if presentation:
            # The command ACK belongs to the SDL/UI thread; the original
            # presentation vote is delivered by the guest thread afterwards.
            # Wait for that actual acknowledgement without repeating intent.
            self.m.wait(lambda: self.ack(role).get('fallback_sequence', 0) > before.get('fallback_sequence', 0)
                        or not self.ack(role).get('presentation') or self.observe(role).get('has_surface'), 4)
        return result


class SettingsPointer:
    def __init__(self, pointer): self.p = pointer; self.m = pointer.m

    def open(self, role='host'):
        self.p.command(role, 'settings on')
        self.m.wait(lambda: self.m.ui(role).get('menu_open') and self.m.ui(role).get('widgets'), 12)
        self.m.delay(.15)
        record(self.p, 'f12_fixture_open', role=role, ui=self.m.ui(role))

    def close(self, role='host'):
        self.p.command(role, 'settings off')
        self.m.wait(lambda: not self.m.ui(role).get('menu_open'), 12)
        self.p.cancel(role)

    @staticmethod
    def intersect(widget):
        rect, clip = widget['rect'], widget['clip']
        assert len(rect) == len(clip) == 4 and all(math.isfinite(float(v)) for v in rect + clip)
        a, b, c, d = max(rect[0], clip[0]), max(rect[1], clip[1]), min(rect[2], clip[2]), min(rect[3], clip[3])
        if c - a < 3 or d - b < 3: return None
        return [a, b, c, d]

    def widget(self, wanted, role='host'):
        # Graphics contains a scrollable child. Use its actual clip rectangle,
        # and never click a synthesized off-screen position.
        for _ in range(20):
            state = self.m.ui(role)
            matches = [w for w in state.get('widgets', []) if w['id'] == wanted]
            if len(matches) == 1:
                widget = matches[0]; clip = self.intersect(widget)
                if widget.get('visible') and widget.get('enabled') and clip:
                    return widget, clip
                if not widget.get('enabled'):
                    raise AssertionError(('Settings control is disabled', wanted, widget))
                direction = 1 if widget['rect'][1] < widget['clip'][1] else -1
            else:
                # Graphics is the initial F12 tab. No other tab is selected by
                # the test; missing controls must not silently become coverage.
                if wanted.startswith('file_'): raise AssertionError(('Missing PNG combo entry', wanted, state))
                direction = -1
            bounds = state.get('clip', [])
            assert len(bounds) == 4 and bounds[2] > bounds[0] and bounds[3] > bounds[1], state
            x, y = (bounds[0] + bounds[2]) / 2, (bounds[1] + bounds[3]) / 2
            self.p.command(role, 'move ' + str(x) + ' ' + str(y))
            self.p.command(role, 'wheel 0 ' + str(direction * 4) + ' ' + str(x) + ' ' + str(y))
            self.m.delay(.15)
        raise AssertionError(('Settings widget did not become hittable', wanted, self.m.ui(role)))

    def click(self, wanted, role='host', fraction=.5):
        widget, bounds = self.widget(wanted, role)
        x, y = bounds[0] + fraction * (bounds[2] - bounds[0]), (bounds[1] + bounds[3]) / 2
        self.p.command(role, 'move ' + str(x) + ' ' + str(y))
        self.p.command(role, 'down left ' + str(x) + ' ' + str(y))
        self.m.delay(.07)
        self.p.command(role, 'up left ' + str(x) + ' ' + str(y))
        self.m.delay(.2)
        record(self.p, 'imgui_pointer_click', role=role, id=wanted, widget=widget, point=[x, y], ui=self.m.ui(role))

    def slider(self, wanted, value, role='host'):
        ranges = {'size': (8, 256), 'opacity': (1, 100)}
        minimum, maximum = ranges[wanted]
        assert minimum <= value <= maximum
        widget, bounds = self.widget(wanted, role)
        # The trace item includes its text label. These two crosshair sliders
        # explicitly draw a 280px ImGui frame; clicks remain in its measured
        # clipped item, never in the adjacent label or another control.
        low = max(widget['rect'][0] + 2, bounds[0] + 1)
        high = min(widget['rect'][0] + 278, bounds[2] - 1)
        assert high > low, widget
        for _ in range(12):
            x = low if value == minimum else high if value == maximum else (low + high) / 2
            widget, bounds = self.widget(wanted, role)
            self.click(wanted, role, fraction=(x - bounds[0]) / (bounds[2] - bounds[0]))
            actual = self.m.ui(role)[wanted]
            if actual == value:
                record(self.p, 'imgui_pointer_slider_exact_value', role=role, id=wanted, value=value)
                return
            if actual < value: low = x + .1
            else: high = x - .1
        raise AssertionError(('Slider did not reach exact value by pointer', wanted, value, self.m.ui(role)))

    def set_native_color(self, on, role='host'):
        value = self.m.ui(role).get('native_color')
        assert isinstance(value, bool), ('Candidate lacks actual color-mode UI observation', self.m.ui(role))
        if value != on: self.click('native', role)
        self.m.wait(lambda: self.m.ui(role).get('native_color') == on and self.m.render(role).get('native_color') == on, 10)
        record(self.p, 'imgui_pointer_native_color_mode', role=role, native_color=on)

    def select_png(self, role='host', index=0, name=PNG_NAME):
        self.click('file', role)
        self.m.wait(lambda: any(w['id'] == 'file_' + str(index) for w in self.m.ui(role).get('widgets', [])), 8)
        self.click('file_' + str(index), role)
        self.m.wait(lambda: self.m.ui(role).get('file') == name and self.m.ui(role).get('enabled'), 10)
        self.set_native_color(False, role)  # Explicitly choose the supplied PNG's own white/black colors.

    def reload(self, role='host'):
        before = self.m.render(role).get('catalog_scans', 0)
        self.click('reload', role)
        self.m.wait(lambda: self.m.render(role).get('catalog_scans', 0) > before, 10)


def core(m, role='host'): return dict(m.render(role).get('core', {}))


def capture(p, label, role='host'):
    if p.m.ui(role).get('menu_open'):
        # UISHOT belongs to the game pad fixture and is intentionally suspended
        # while ImGui owns input. JSON records UI/error state; gameplay images
        # are captured only after F12 has genuinely closed.
        record(p, 'f12_open_screenshot_not_claimed', role=role, label=label, ui=p.m.ui(role))
        return None
    folder = p.m.folder / role / 'steps'
    def inventory():
        return {path.name: (path.stat().st_mtime_ns, path.stat().st_size)
                for path in folder.glob('ui-*.png')}
    before = inventory()
    p.m.press(role, 'UISHOT')
    p.m.wait(lambda: any(before.get(name) != value for name, value in inventory().items()), 10)
    changed = [folder / name for name, value in inventory().items() if before.get(name) != value]
    source = max(changed, key=lambda path: path.stat().st_mtime_ns)
    p.m.delay(.1)
    assert source.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), ('Incomplete native screenshot', str(source))
    evidence = p.m.folder / 'evidence'; evidence.mkdir(exist_ok=True)
    target = evidence / (label + '-' + role + '.png'); shutil.copy2(source, target)
    metadata = source.with_suffix('.json')
    if metadata.exists(): shutil.copy2(metadata, target.with_suffix('.json'))
    return str(target)


def quiet_replacements(p, label, role='host', seconds=1):
    p.m.delay(.5)  # Drain already queued FIFO packets before measuring fallback.
    before = core(p.m, role); p.m.delay(seconds); after = core(p.m, role)
    assert before.get('replacements') == after.get('replacements'), (label, before, after, p.m.diagnostics(role))
    screenshot = capture(p, label, role)
    record(p, label, role=role, before=before, after=after, screenshot=screenshot)


def loaded(p, role='host'):
    p.m.wait(lambda: p.m.render(role).get('enabled') and p.m.render(role).get('ready')
             and p.m.render(role).get('image_loaded') and p.m.render(role).get('hook_supported'), 15)
    state = p.m.render(role)
    assert [state.get(k) for k in ('width', 'height', 'left', 'top', 'right', 'bottom')] == [256, 256, 25, 153, 39, 167], state
    assert state.get('preview_ready') and not state.get('error'), state
    record(p, 'user_png_loaded_and_alpha_trimmed', role=role, state=state)


def original(p, settings, role='host'):
    settings.click('original', role)
    p.m.wait(lambda: not p.m.ui(role).get('enabled') and not p.m.render(role).get('ready'), 10)
    assert p.m.ui(role).get('native_color') is True, ('Original did not restore native color default', p.m.ui(role))
    quiet_replacements(p, 'original_restored_without_replacement', role)


def replacement(p, label, role='host'):
    before = core(p.m, role)
    p.m.wait(lambda: core(p.m, role).get('replacements', 0) > before.get('replacements', 0), 25)
    after = core(p.m, role)
    # last_replaced is transient and may already describe a later ordinary
    # draw when the renderer diagnostic is sampled; growing counters prove use.
    assert after.get('tags', 0) > 0 and after.get('last_primitive') == 0x80, after
    for key in ('original', 'drawn'):
        values = [after.get(key + '_' + side) for side in ('left', 'top', 'right', 'bottom')]
        assert all(isinstance(value, (int, float)) and math.isfinite(value) for value in values), after
        assert values[2] > values[0] and values[1] > values[3], after
    for axis in (('left', 'right'), ('top', 'bottom')):
        old = sum(after['original_' + side] for side in axis) / 2
        new = sum(after['drawn_' + side] for side in axis) / 2
        assert abs(old - new) < .0001, ('Replacement drifted from original aimed center', axis, after)
    assert after.get('rejected', 0) == 0 and after.get('dropped', 0) == 0, after
    screenshot = capture(p, label, role)
    record(p, label, role=role, render=p.m.render(role), native=p.m.crosshair(role), menu=public_menu(p.m.state(role)), screenshot=screenshot)


def start_offline(p):
    p.expect_root(0x3d); p.click(0x3e, root=0x3d); select_original_profile(p, footer=True)
    p.expect_root(0x4b); p.m.wait(lambda: len(p.m.carousel().get('surfaces', [])) == 1, 15)
    state = p.m.carousel()['surfaces'][0]
    entries = [entry for entry in state.get('entries', []) if entry.get('level') == 'Rlevel1_1']
    assert len(entries) == 1, ('No authored first campaign level; no forced load substituted', state)
    target = ident(entries[0]['index'])
    for _ in range(ident(state['count'])):
        current = ident(p.m.carousel()['surfaces'][0]['selected'])
        if current == target: break
        step = 1 if current < target else -1
        p.wheel(p.center(p.widget('carousel.confirm', root=0x4b)), -step)
        p.m.wait(lambda: ident(p.m.carousel()['surfaces'][0]['selected']) == current + step, 8)
    assert p.m.carousel()['surfaces'][0]['selected_level'] == 'Rlevel1_1'
    p.click('carousel.confirm', root=0x4b); p.expect_root(0x44); p.click(0x47, root=0x44)
    p.m.wait(lambda: p.m.state().get('scene') == 4 and 'IMPORT_Read_Old(levels/Rlevel1_1.ssw) success' in p.m.log('host'), 90)
    assert not p.m.state().get('active'), p.m.state()
    # End only the ordinary initial Info/intro via original owned mouse paths.
    deadline = time.monotonic() + 60; stable_since = None; saw_presentation = False
    while True:
        now = time.monotonic()
        if now > deadline: raise TimeoutError('Offline authored introduction did not retire ' + str(p.m.diagnostics()))
        presentation = bool(p.ack().get('presentation')); saw_presentation |= presentation
        native = p.observe(); info = bool(p.m.state().get('info'))
        if not native.get('has_surface') and not presentation and not info:
            if stable_since is None: stable_since = now
            # A brief playable first frame precedes the authored intro; require
            # sustained retirement so its later start cannot pass as gameplay.
            if now - stable_since >= 4: break
            p.m.delay(.1); continue
        stable_since = None
        bodies = [w for surface in native.get('surfaces', []) for w in surface.get('widgets', [])
                  if w.get('control') == 'info.body' and w.get('allowed')]
        if bodies: p.click_uv(p.center(bodies[0]), button='right')
        elif presentation and not native.get('has_surface'): p.click_uv([.98, .05])
        else: p.m.delay(.25)
    record(p, 'offline_authored_intro_retired_stably', observed_presentation=saw_presentation, stable_seconds=4)
    record(p, 'authored_offline_gameplay_started_by_native_pointer', level='Rlevel1_1', menu=public_menu(p.m.state()))


def ui_case(p, settings):
    assert not p.m.ui().get('enabled') and not p.m.render().get('enabled'), p.m.diagnostics()
    assert core(p.m).get('replacements', 0) == 0
    settings.open(); settings.click('enabled')
    p.m.wait(lambda: p.m.ui().get('enabled'), 8)
    assert not p.m.render().get('ready'), 'Empty selection must keep original reticle'
    settings.select_png(); loaded(p)
    scans, loads = p.m.render()['catalog_scans'], p.m.render()['image_loads']
    p.m.delay(.8)
    assert (p.m.render()['catalog_scans'], p.m.render()['image_loads']) == (scans, loads), 'Idle rendering repeatedly reads disk'
    settings.reload(); loaded(p)
    assert p.m.render()['image_loads'] > loads
    settings.click('size', fraction=.18); settings.click('opacity', fraction=.58)
    saved = {key: p.m.ui()[key] for key in ('enabled', 'file', 'size', 'opacity', 'native_color')}
    assert saved['size'] != 32 and saved['opacity'] != 100, saved
    settings.close(); quiet_replacements(p, 'custom_does_not_draw_over_main_menu')
    p.m.press('host', 'UISHOT'); p.m.stop_role('host'); p.m.start_role('host', p.m.a.language)
    p.expect_root(0x3d); loaded(p)
    assert {key: p.m.ui()[key] for key in saved} == saved, ('Settings failed actual process restart persistence', saved, p.m.ui())
    record(p, 'actual_process_restart_persisted_pointer_settings', saved=saved,
           settings_sha256=sha(p.m.folder / 'host/settings.ini'))
    settings.open(); original(p, settings); settings.select_png(); loaded(p); settings.close()
    return saved


def quiet_visual_setup(p, settings):
    # Explicit private screenshot setup: no production config/card is changed.
    p.m.press('host', 'GODON')
    p.m.delay(.25)
    record(p, 'private_gameplay_screenshot_setup', token='GODON',
           reason='Avoid damage flashes during same-camera Original/PNG comparison; NPCs can still move.')
    settings.open(); settings.slider('size', 14); settings.slider('opacity', 100)
    original(p, settings); settings.close()
    quiet_replacements(p, 'size14_original_same_view')
    settings.open(); settings.select_png(); loaded(p); settings.close()
    replacement(p, 'size14_custom_same_view')


def gameplay_fallbacks(p, settings):
    start_offline(p); quiet_visual_setup(p, settings)
    path = p.m.folder / 'host/crosshairs' / PNG_NAME
    original_bytes = path.read_bytes()
    settings.open()
    try:
        missing = path.with_suffix('.png.missing'); path.rename(missing)
        settings.reload()
        p.m.wait(lambda: not p.m.render().get('ready') and not p.m.render().get('image_loaded') and bool(p.m.render().get('error')), 10)
        assert 'missing' in p.m.render()['error'].lower(), p.m.render()
        record(p, 'missing_png_actionable_error', error=p.m.render()['error'], screenshot=capture(p, 'missing_png_message'))
        settings.close(); quiet_replacements(p, 'missing_png_keeps_original_native_reticle')
        missing.rename(path); settings.open(); settings.reload(); loaded(p); settings.close()
        replacement(p, 'missing_png_recovery_replaces_native_reticle')
        path.write_bytes(b'not a PNG\x00bounded private corrupt fixture')
        settings.open(); settings.reload()
        p.m.wait(lambda: not p.m.render().get('ready') and not p.m.render().get('image_loaded') and bool(p.m.render().get('error')), 10)
        assert 'not a png' in p.m.render()['error'].lower(), p.m.render()
        record(p, 'corrupt_png_actionable_error', error=p.m.render()['error'], screenshot=capture(p, 'corrupt_png_message'))
        settings.close(); quiet_replacements(p, 'corrupt_png_keeps_original_native_reticle')
        path.write_bytes(original_bytes); settings.open(); settings.reload(); loaded(p); settings.close()
        replacement(p, 'corrupt_png_recovery_replaces_native_reticle')
        settings.open(); original(p, settings); settings.close()
        quiet_replacements(p, 'native_gameplay_original_button_disables_replacement')
    finally:
        path.write_bytes(original_bytes)
        missing = path.with_suffix('.png.missing')
        if missing.exists(): missing.unlink()  # Exact private fixture file only.


def software_case(p, settings):
    settings.open(); settings.select_png(); loaded(p); settings.close()
    quiet_replacements(p, 'software_custom_does_not_draw_over_main_menu')
    start_offline(p); quiet_visual_setup(p, settings)
    replacement(p, 'software_original_reticle_primitive_replaced')
    settings.open(); original(p, settings); settings.close()
    quiet_replacements(p, 'software_native_original_fallback')


def direct_gameplay_setup(p):
    p.m.start_role('client', p.m.a.client_language)
    record(p, 'declared_cli_gameplay_setup', level='Rlevel1_1',
           host_flags=['--coop-host', '--coop-capacity', '8', '--coop-level', 'Rlevel1_1'],
           client_flags=['--coop-join', '127.0.0.1'], automatic_original_intro_fixture=True,
           scope='Setup fixture only. This case does not claim native-menu host creation, LAN discovery, map choice or mouse intro-vote coverage.')
    p.m.wait(lambda: all(p.m.state(role).get('scene') == 4 and p.m.performance(role).get('local_ready')
                        and p.m.performance(role).get('ready_players') == 2 and p.m.performance(role).get('level') == 'Rlevel1_1'
                        and p.m.ui(role) and p.m.render(role) for role in ('host', 'client')), 120)
    close_initial_information_by_mouse(p)
    p.m.wait(lambda: all(not p.m.state(role).get('info') and not p.observe(role).get('has_surface')
                        and not p.ack(role).get('presentation') for role in ('host', 'client')), 75)
    p.m.delay(2)
    record(p, 'cli_fixture_both_original_gameplay_ready', performance={role:p.m.performance(role) for role in ('host','client')})


def multiplayer_case(p, settings):
    direct = p.m.a.case == 'host-client-direct'
    if direct:
        direct_gameplay_setup(p)
        p.m.press('host', 'GODON')
    settings.open(); settings.select_png(); loaded(p)
    settings.slider('size', 14); settings.slider('opacity', 100); settings.close()
    if not direct:
        host_create(p); client_join_and_roles(p)
        begin_campaign(p)
    assert not p.m.ui('client').get('enabled') and not p.m.render('client').get('ready'), p.m.diagnostics('client')
    p.m.press('host', 'GODON')  # Private screenshot setup; avoids incidental deaths while editing client F12.
    record(p, 'private_multiplayer_screenshot_setup', token='GODON', scope='Only private host gameplay; no card/source settings written.')
    replacement(p, 'host_custom_reticle_in_shared_native_gameplay')
    quiet_replacements(p, 'client_local_original_reticle_is_independent', 'client')
    assert core(p.m, 'client').get('replacements', 0) == 0
    record(p, 'host_on_client_off_same_gameplay', host=p.m.diagnostics(), client=p.m.diagnostics('client'))
    # Exercise the client feature itself, rather than inferring its support
    # from the host. These are measured real F12 pointer widgets, not fields.
    settings.open('client'); settings.select_png('client'); loaded(p, 'client')
    settings.slider('size', 14, 'client'); settings.slider('opacity', 100, 'client'); settings.close('client')
    replacement(p, 'client_custom_reticle_in_shared_native_gameplay', 'client')
    replacement(p, 'host_custom_continues_after_client_local_change')
    settings.open(); settings.set_native_color(True); settings.close()
    replacement(p, 'host_native_health_target_color_enabled')
    assert p.m.render().get('native_color') is True and p.m.render('client').get('native_color') is False
    record(p, 'native_color_mode_is_local', host=p.m.render(), client=p.m.render('client'))
    settings.open(); original(p, settings); settings.close()
    quiet_replacements(p, 'host_original_client_custom_is_independent')
    replacement(p, 'client_custom_continues_after_host_original', 'client')
    assert not p.m.render().get('ready') and p.m.render('client').get('ready')
    record(p, 'host_off_client_on_same_gameplay', host=p.m.diagnostics(), client=p.m.diagnostics('client'))
    if not direct:
        leave_sessions(p)
        record(p, 'both_original_session_leave_paths_completed', scope='One authored level smoke; no full campaign claim.')
    else:
        record(p, 'direct_fixture_cleanup_uses_private_quit', scope='No native menu Join/Leave or full campaign claim. HLE restore/rehost is covered by the actual scope helper unit test.')


def arguments():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'data', 'card', 'png', 'output'): ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--case', choices=('gpu-ui', 'software-gameplay', 'host-client', 'host-client-direct', 'all'), default='all')
    ap.add_argument('--language', type=int, choices=(0, 1, 2), default=1)
    ap.add_argument('--client-language', type=int, choices=(0, 1, 2), default=2)
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--port', type=int, default=28540)
    a = ap.parse_args()
    for name in ('exe', 'data', 'card', 'png', 'output'): setattr(a, name, getattr(a, name).resolve())
    assert all(str(getattr(a, name)).isascii() for name in ('exe', 'data', 'card', 'png', 'output')), 'Use private ASCII runtime paths'
    assert a.exe.is_file() and a.card.is_file() and a.png.is_file() and (a.data / 'sys/main.dol').is_file()
    assert 1024 < a.port < 65528 and a.fps > 0
    assert a.png.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), 'Expected the supplied original PNG asset'
    a.allow_start_key_fallback = False
    return a


def run_case(a, name, report):
    renderer = 'software' if name == 'software-gameplay' else 'gpu'
    a.private_settings = ('[graphics]\nrenderer=' + renderer + '\nscale=1\naspect=3\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w=960\nwindow_h=540\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\npause_in_menu=0\n')
    case = {'case': name, 'renderer': renderer, 'passed': False, 'steps': []}; report['cases'].append(case)
    m = CrosshairMenus(a, '', a.output / name); p = CrosshairPointer(m, case); settings = SettingsPointer(p)
    try:
        m.start_role('host', a.language)
        if name != 'host-client-direct':
            p.expect_root(0x3d); p.cancel()
        if name == 'gpu-ui': ui_case(p, settings); gameplay_fallbacks(p, settings)
        elif name == 'software-gameplay': software_case(p, settings)
        else: multiplayer_case(p, settings)
        case['passed'] = True
    except Exception:
        case['error'] = traceback.format_exc(); print(case['error'], flush=True)
        case['failure_diagnostics'] = {role: m.diagnostics(role) for role in m.processes}
        for role, child in m.processes.items():
            if child.poll() is None:
                try: m.press(role, 'UISHOT')
                except Exception: pass
    finally:
        # The game-pad QUIT fixture is also suspended by F12. Retire F12 via
        # its existing pointer adapter before asking the private child to quit.
        for role, child in m.processes.items():
            if child.poll() is None:
                try: settings.close(role)
                except Exception: pass
        m.close()
        case.update(visible=m.visible, focused=m.focused,
                    launches=[{'role': entry['role'], 'generation': entry['generation'],
                               'pid': entry['process'].pid, 'exit': entry['process'].returncode} for entry in m.launches],
                    final={role: m.diagnostics(role) for role in m.processes})
        case['passed'] &= bool(m.launches) and not m.visible and not m.focused and all(
            entry['process'].returncode == 0 for entry in m.launches)
        (m.folder / 'result.json').write_text(json.dumps(case, indent=2) + '\n', encoding='utf8')
    return case['passed']


def main():
    a = arguments(); a.output.mkdir(parents=True, exist_ok=False)
    before = source_state(a)
    report = {'passed': False, 'scope': __doc__, 'exe_sha256': before['exe'], 'png_sha256': before['png'],
              'driver_sha256': sha(Path(__file__)), 'case': a.case, 'fps': a.fps, 'aspect': '16:9', 'cases': []}
    try:
        cases = ('gpu-ui', 'software-gameplay', 'host-client') if a.case == 'all' else (a.case,)
        for index, name in enumerate(cases):
            a.port += index * 2
            if not run_case(a, name, report): break
        report['passed'] = len(report['cases']) == len(cases) and all(case['passed'] for case in report['cases'])
    except Exception:
        report['error'] = traceback.format_exc(); print(report['error'], flush=True)
    finally:
        after = source_state(a)
        report['source_files_unchanged'] = {key: after[key] == before[key] for key in before}
        report['passed'] &= all(report['source_files_unchanged'].values())
        (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
        print(json.dumps({key: report[key] for key in ('passed', 'case', 'source_files_unchanged')}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
