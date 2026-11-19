import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
CONTROLS = {
    "ADCL Input MIC1 Boost Switch": "off",
    "MIC1 gain volume": "4",
    "ADC volume": "190,155",
}


class MixerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="chh-test-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.app = self.base / "App with spaces"
        self.app.mkdir()
        shutil.copyfile(SOURCE / "control.sh", self.app / "control.sh")
        (self.app / "control.sh").chmod(0o755)
        self.root = self.base / "root"
        files = {
            "usr/trimui/bin/MainUI": "unrelated string\nTrimui Smart Pro\n",
            "proc/sys/kernel/osrelease": "4.9.191\n",
            "proc/sys/kernel/random/boot_id": "aaaabbbb-cccc-dddd-eeee-000011112222\n",
            "sys/firmware/devicetree/base/compatible": "allwinner,a133\0arm,sun50iw10p1\0",
            "mnt/SDCARD/.system/tg5040/bin/run_hooks.sh": "# fake hook runner\n",
            "mnt/SDCARD/.system/version.txt": "NextUI-test\n",
            "proc/meminfo": "MemTotal: 1024000 kB\nMemAvailable: 600000 kB\n",
            "proc/asound/cards": "0 [audiocodec]: audiocodec\n",
            "proc/asound/card0/id": "audiocodec\n",
            "proc/asound/card0/pcm0c/info": "card: 0\ndevice: 0\nname: codec capture\n",
            "dev/snd/controlC0": "",
            "proc/cpuinfo": "Hardware: test A133\nSerial\t: PRIVATE-SERIAL\n",
            "mnt/SDCARD/.userdata/tg5040/auto.sh": "#!/bin/sh\n# user's existing startup\n",
        }
        for name, value in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value)
        self.hook = self.root / "mnt/SDCARD/.userdata/tg5040/.hooks/boot.d/90-choochoohacker.sh"
        self.original = self.root / "mnt/SDCARD/.userdata/tg5040/auto.sh"
        self.original_bytes = self.original.read_bytes()
        self.mixer_file = self.base / "mixer.json"
        self.mixer_file.write_text(json.dumps(CONTROLS))
        self.fail_file = self.base / "fail-once"
        self.log = self.base / "writes"
        binary = self.base / "bin"
        binary.mkdir()
        fake = binary / "amixer"
        fake.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, pathlib, sys\n"
            "path = pathlib.Path(os.environ['FAKE_MIXER'])\n"
            "state = json.loads(path.read_text())\n"
            "controls = [('ADCL Input MIC1 Boost Switch', 'BOOLEAN', '1'), ('MIC1 gain volume', 'INTEGER', '1'), ('ADC volume', 'INTEGER', '2'), ('MIC Speaker Playback volume', 'INTEGER', '1'), ('Capture Source', 'ENUMERATED', '1')]\n"
            "if len(sys.argv) > 3 and sys.argv[3] == 'controls':\n"
            "    for i, (name, _, _) in enumerate(controls, 1):\n"
            "        if name in state: print(f\"numid={i},iface=MIXER,name='{name}'\")\n"
            "    sys.exit(0)\n"
            "if len(sys.argv) < 5: print('fake amixer: unavailable command', file=sys.stderr); sys.exit(2)\n"
            "numbered = sys.argv[4].startswith('numid=')\n"
            "if numbered:\n"
            "    try: number = int(sys.argv[4][6:]); name, kind, count = controls[number-1]\n"
            "    except (ValueError, IndexError): sys.exit(2)\n"
            "else: name = sys.argv[4][len('name='):]\n"
            "if name not in state: sys.exit(2)\n"
            "if sys.argv[3] == 'cget':\n"
            "    if numbered:\n"
            "        print(f\"numid={number},iface=MIXER,name='{name}'\")\n"
            "        extra = ',items=3' if kind == 'ENUMERATED' else ',min=0,max=' + ('255' if name == 'ADC volume' else '31')\n"
            "        print(f'  ; type={kind},access=rw------,values={count}' + extra)\n"
            "    print('  : values=' + state[name])\n"
            "else:\n"
            "    fail = pathlib.Path(os.environ['FAKE_FAIL'])\n"
            "    if fail.exists() and fail.read_text() == 'disable' and name == 'ADCL Input MIC1 Boost Switch' and sys.argv[5] == 'off':\n"
            "        fail.unlink(); print('injected disable failure', file=sys.stderr); sys.exit(5)\n"
            "    if fail.exists() and fail.read_text() == 'restore' and name == 'ADC volume' and sys.argv[5] == '190,155':\n"
            "        fail.unlink(); print('injected restore failure', file=sys.stderr); sys.exit(5)\n"
            "    if fail.exists() and name == 'MIC1 gain volume' and sys.argv[5] == '31':\n"
            "        fail.unlink(); print('injected mixer failure', file=sys.stderr); sys.exit(5)\n"
            "    state[name] = sys.argv[5]\n"
            "    path.write_text(json.dumps(state))\n"
            "    with open(os.environ['FAKE_LOG'], 'a') as log: log.write(name + '=' + sys.argv[5] + '\\n')\n"
        )
        fake.chmod(0o755)
        self.env = dict(os.environ, CHH_ROOT=str(self.root), FAKE_MIXER=str(self.mixer_file),
                        FAKE_FAIL=str(self.fail_file), FAKE_LOG=str(self.log), PLATFORM="tg5040",
                        PATH=str(binary) + os.pathsep + os.environ["PATH"])

    def run_action(self, action, success=True, args=()):
        result = subprocess.run(["sh", str(self.app / "control.sh"), action, *args],
                                env=self.env, capture_output=True, text=True, timeout=20)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.original.read_bytes(), self.original_bytes)
        return result

    def values(self):
        return json.loads(self.mixer_file.read_text())

    def set_profile(self, profile):
        self.env["PLATFORM"] = profile
        for candidate in ("tg5040", "tg5050"):
            runner = self.root / f"mnt/SDCARD/.system/{candidate}/bin/run_hooks.sh"
            runner.parent.mkdir(parents=True, exist_ok=True)
            if candidate == profile:
                runner.write_text("# fake hook runner\n")
            elif runner.exists():
                runner.unlink()
        model = "Trimui Smart Pro S" if profile == "tg5050" else "Trimui Smart Pro"
        (self.root / "usr/trimui/bin/MainUI").write_text(model + "\n")
        self.hook = self.root / f"mnt/SDCARD/.userdata/{profile}/.hooks/boot.d/90-choochoohacker.sh"

    def prepare_usb_gadget(self):
        usb_audio = self.app / "usb-audio"
        usb_audio.mkdir()
        (usb_audio / "u_audio.ko").write_bytes(b"test helper module")
        (usb_audio / "usb_f_uac1.ko").write_bytes(b"test UAC1 module")
        root = self.root
        config = root / "sys/kernel/config/usb_gadget/g1/configs/c.1"
        functions = root / "sys/kernel/config/usb_gadget/g1/functions"
        ffs = functions / "ffs.adb"
        uac1 = functions / "uac1.usb0"
        udc = root / "sys/kernel/config/usb_gadget/g1/UDC"
        state = root / "sys/class/udc/5100000.udc-controller/state"
        role = root / "sys/devices/platform/soc/usbc0/otg_role"
        cards = root / "proc/asound/cards"
        modules = root / "proc/modules"
        for path in (config, ffs, uac1, state.parent, role.parent, cards.parent):
            path.mkdir(parents=True, exist_ok=True)
        (config / "ffs.adb").symlink_to(ffs, target_is_directory=True)
        udc.write_text("5100000.udc-controller\n")
        state.write_text("configured\n")
        role.write_text("usb_device\n")
        modules.write_text("snd 123 0 - Live 0\n")
        self.env.update(
            FAKE_USB_LOG=str(self.base / "usb-actions"),
            FAKE_USB_CONFIG=str(config),
            FAKE_USB_FFS=str(ffs),
            FAKE_USB_UAC1=str(uac1),
            FAKE_USB_UDC=str(udc),
            FAKE_USB_STATE=str(state),
            FAKE_USB_CARDS=str(cards),
            FAKE_MODULES=str(modules),
        )
        fake_insmod = self.base / "bin/insmod"
        fake_insmod.write_text(
            "#!/bin/sh\n"
            "name=$(basename \"$1\" .ko)\n"
            "printf '%s 123 0 - Live 0\\n' \"$name\" >> \"$FAKE_MODULES\"\n"
            "printf 'insmod=%s\\n' \"$name\" >> \"$FAKE_USB_LOG\"\n"
        )
        fake_insmod.chmod(0o755)
        fake_sleep = self.base / "bin/sleep"
        fake_sleep.write_text(
            "#!/bin/sh\n"
            "[ ! -e \"$FAKE_USB_DELAY_ENUMERATION\" ] || echo configured > \"$FAKE_USB_STATE\"\n"
        )
        fake_sleep.chmod(0o755)
        self.env["FAKE_USB_DELAY_ENUMERATION"] = str(self.base / "delay-usb-enumeration")
        adbd = root / "etc/init.d/adbd"
        adbd.parent.mkdir(parents=True, exist_ok=True)
        adbd.write_text(
            "#!/bin/sh\n"
            "printf 'adbd:%s\\n' \"$1\" >> \"$FAKE_USB_LOG\"\n"
            "case \"$1\" in\n"
            "  stop) : ;;\n"
            "  start) [ ! -e \"$FAKE_USB_FAIL_ADBD_START\" ] || exit 8; "
            "rm -f \"$FAKE_USB_CONFIG/ffs.adb\" \"$FAKE_USB_CONFIG/uac1.usb0\"; "
            "ln -s \"$FAKE_USB_FFS\" \"$FAKE_USB_CONFIG/ffs.adb\"; "
            "printf '5100000.udc-controller\\n' > \"$FAKE_USB_UDC\"; "
            "echo configured > \"$FAKE_USB_STATE\" ;;\n"
            "  *) exit 2 ;;\n"
            "esac\n"
        )
        adbd.chmod(0o755)
        self.env["FAKE_USB_FAIL_ADBD_START"] = str(self.base / "fail-adbd-start")
        helper = root / "bin/setusbconfig"
        helper.parent.mkdir(parents=True, exist_ok=True)
        helper.write_text(
            "#!/bin/sh\n"
            "printf 'setusbconfig:%s\\n' \"$1\" >> \"$FAKE_USB_LOG\"\n"
            "[ \"$1\" = uac1 ] || exit 2\n"
            "[ ! -e \"$FAKE_USB_FAIL_HELPER\" ] || exit 7\n"
            "[ -n \"$(cat \"$FAKE_USB_UDC\")\" ] || exit 9\n"
            "rm -f \"$FAKE_USB_CONFIG/ffs.adb\" \"$FAKE_USB_CONFIG/uac1.usb0\"\n"
            "ln -s \"$FAKE_USB_UAC1\" \"$FAKE_USB_CONFIG/uac1.usb0\"\n"
            "printf '5100000.udc-controller\\n' > \"$FAKE_USB_UDC\"\n"
            "if [ -e \"$FAKE_USB_DELAY_ENUMERATION\" ]; then "
            "echo 'not attached' > \"$FAKE_USB_STATE\"; "
            "else echo configured > \"$FAKE_USB_STATE\"; fi\n"
            "echo '1 [UAC1Gadget]: UAC1_Gadget' >> \"$FAKE_USB_CARDS\"\n"
        )
        helper.chmod(0o755)
        self.env["FAKE_USB_FAIL_HELPER"] = str(self.base / "fail-helper")

    def run_cli(self, *args, code=0):
        executable = self.app / "choochoohacker"
        if not executable.exists():
            shutil.copyfile(SOURCE / "build/ui-test", executable)
            executable.chmod(0o755)
        env = dict(self.env, SDL_VIDEODRIVER="invalid", SDL_AUDIODRIVER="invalid")
        result = subprocess.run([str(executable), *args], cwd=self.base, env=env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result

    def test_cli_status_is_machine_readable_without_display_or_trusted_cwd(self):
        (self.base / "control.sh").write_text("echo WRONG_BACKEND; exit 99\n")
        result = self.run_cli("status")
        fields = dict(line.split("=", 1) for line in result.stdout.splitlines())
        self.assertEqual(fields["MODEL"], "Trimui Smart Pro")
        self.assertEqual(fields["GAIN"], "4")
        self.assertEqual(result.stderr, "")
        self.assertFalse(self.log.exists())
        self.assertFalse((self.app / "state").exists())

    def test_cli_actions_share_backend_and_restore_exact_values(self):
        self.run_cli("mic-on")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.run_action("cleanup")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.run_cli("restore")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())

    def test_cli_reports_usage_and_backend_errors_on_stderr(self):
        self.run_cli("status", "extra", code=2)
        self.run_cli("boot", code=2)
        (self.root / "usr/trimui/bin/MainUI").write_text("Unknown handheld\n")
        result = self.run_cli("mic-on", code=1)
        self.assertIn("ERROR:", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.log.exists())

    def test_cli_missing_adjacent_backend_does_not_run_cwd_script(self):
        (self.app / "control.sh").unlink()
        (self.base / "control.sh").write_text("echo WRONG_BACKEND; exit 99\n")
        result = self.run_cli("status", code=1)
        self.assertIn("Cannot read backend script", result.stderr)
        self.assertEqual(result.stdout, "")
        self.run_cli("--help")

    def test_diagnostic_never_writes(self):
        status = self.run_action("status")
        self.assertIn("MODEL=Trimui Smart Pro", status.stdout)
        self.assertIn("GAIN=4", status.stdout)
        self.assertFalse(self.log.exists())
        self.assertFalse((self.app / "state").exists())

    def test_temporary_survives_exit_until_manual_restore(self):
        self.run_action("mic-on")
        self.run_action("mic-on")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.run_action("cleanup")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        status = self.run_action("status")
        self.assertIn("PERSIST=disabled", status.stdout)
        self.assertIn("TEMPORARY=until-restart", status.stdout)
        self.run_action("restore")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/mic-baseline").exists())
        self.assertFalse(self.hook.exists())

    def test_temporary_hook_restores_at_next_boot_and_removes_itself(self):
        self.run_action("mic-on")
        self.run_action("cleanup")
        result = subprocess.run(["sh", str(self.hook)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (self.app / "boot.log").read_text())
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())
        self.assertFalse((self.app / "state/mic-baseline").exists())

    def test_temporary_reset_failure_keeps_hook_and_baseline_for_retry(self):
        self.run_action("mic-on")
        self.fail_file.write_text("restore")
        result = self.run_action("reset-temp", success=False)
        self.assertIn("Restore incomplete", result.stderr)
        self.assertTrue(self.hook.exists())
        self.assertTrue((self.app / "state/mic-baseline").exists())
        self.run_action("reset-temp")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())

    def test_temporary_hook_install_failure_rolls_back(self):
        self.hook.parent.parent.mkdir(parents=True)
        self.hook.parent.write_text("not a directory")
        result = self.run_action("mic-on", success=False)
        self.assertIn("Cannot install boot hook", result.stderr)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/mic-baseline").exists())

    def test_temporary_can_become_persistent_without_losing_original_values(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        self.assertIn("PERSIST=enabled", self.run_action("status").stdout)
        self.run_action("cleanup")
        result = subprocess.run(["sh", str(self.hook)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (self.app / "boot.log").read_text())
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.assertTrue(self.hook.exists())
        self.run_action("persist-off")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())

    def test_activation_failure_rolls_back(self):
        self.fail_file.touch()
        result = self.run_action("mic-on", success=False)
        self.assertIn("rolling back", result.stderr)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/mic-baseline").exists())
        self.assertFalse((self.app / "state/lock").exists())
        self.assertFalse(self.hook.exists())

    def test_persistence_hook_and_removal(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        self.assertTrue(self.hook.exists())
        self.run_action("cleanup")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.mixer_file.write_text(json.dumps(CONTROLS))
        result = subprocess.run(["sh", str(self.hook)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (self.app / "boot.log").read_text())
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.run_action("mic-on")
        self.assertIn("PERSIST=enabled", self.run_action("status").stdout)
        self.run_action("persist-off")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())
        self.run_action("persist-off")

    def test_default_setup_installs_on_boot_for_tg5040(self):
        self.assert_default_setup("tg5040")

    def test_default_setup_installs_on_boot_for_tg5050(self):
        self.assert_default_setup("tg5050")

    def assert_default_setup(self, profile):
        self.set_profile(profile)
        self.run_action("setup-default")
        self.assertEqual(self.values()["ADCL Input MIC1 Boost Switch"], "on")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.assertEqual(self.values()["ADC volume"], "160,160")
        self.assertTrue(self.hook.exists())
        self.assertIn("mic-default.log", self.hook.read_text())
        self.assertTrue((self.app / "state/mic-default-configured").exists())
        self.assertIn("PERSIST=enabled", self.run_action("status").stdout)

        self.mixer_file.write_text(json.dumps(CONTROLS))
        result = subprocess.run(["sh", str(self.hook)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (self.app / "mic-default.log").read_text())
        self.assertEqual(self.values()["MIC1 gain volume"], "31")
        self.assertIn("MIC1 route set to on", (self.app / "mic-default.log").read_text())

    def test_default_setup_is_idempotent_and_preserves_explicit_disable(self):
        self.run_action("setup-default")
        self.run_action("mic-off")
        self.run_action("setup-default")
        self.assertEqual(self.values()["ADCL Input MIC1 Boost Switch"], "off")
        self.assertEqual((self.app / "state/mic-target").read_text().strip(), "off")
        self.mixer_file.write_text(json.dumps(CONTROLS))
        self.run_action("boot")
        self.assertEqual(self.values()["MIC1 gain volume"], "0")

    def test_persist_off_disables_future_default_setup(self):
        self.run_action("setup-default")
        self.run_action("persist-off")
        self.run_action("setup-default")
        self.assertFalse(self.hook.exists())
        self.assertEqual(self.values(), CONTROLS)
        self.assertTrue((self.app / "state/mic-default-configured").exists())

    def test_default_setup_rolls_back_on_activation_failure(self):
        self.fail_file.touch()
        result = self.run_action("setup-default", success=False)
        self.assertIn("rolling back", result.stderr)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.hook.exists())
        self.assertFalse((self.app / "state/mic-default-configured").exists())

    def test_renamed_install_migrates_state_and_updates_its_boot_hook(self):
        legacy = self.root / "mnt/SDCARD/Tools/tg5040/ChooChooHacker.pak"
        legacy_state = legacy / "state"
        legacy_state.mkdir(parents=True)
        (legacy_state / "mic-baseline").write_text(
            "off\n4\n190,155\n"
        )
        (legacy_state / "mic-target").write_text("on\n")
        self.hook.parent.mkdir(parents=True)
        self.hook.write_text(
            "#!/bin/sh\n"
            "# ChooChooHacker owned MIC1 hook v1\n"
            "CHH_HOME='/mnt/SDCARD/Tools/tg5040/ChooChooHacker.pak'\n"
            "exec \"$CHH_HOME/control.sh\" boot >> \"$CHH_HOME/boot.log\" 2>&1\n"
        )

        self.run_action("setup-default")

        new_state = self.app / "state"
        self.assertEqual((new_state / "mic-baseline").read_text(), "off\n4\n190,155\n")
        self.assertEqual((new_state / "mic-target").read_text(), "on\n")
        self.assertTrue((new_state / "legacy-migration-complete").exists())
        self.assertIn(str(self.app), self.hook.read_text())
        self.assertNotIn("Tools/tg5040/ChooChooHacker.pak", self.hook.read_text())
        self.assertTrue((legacy_state / "mic-baseline").exists())

        self.mixer_file.write_text(json.dumps(CONTROLS))
        result = subprocess.run(["sh", str(self.hook)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (self.app / "boot.log").read_text())
        self.assertEqual(self.values()["MIC1 gain volume"], "31")

    def test_renamed_install_refuses_to_copy_state_while_old_app_is_locked(self):
        legacy_state = self.root / "mnt/SDCARD/Tools/tg5040/ChooChooHacker.pak/state"
        (legacy_state / "lock").mkdir(parents=True)
        result = self.run_action("setup-default", success=False)
        self.assertIn("Close the old ChooChooHacker", result.stderr)
        self.assertFalse((self.app / "state/mic-baseline").exists())
        self.assertFalse((self.app / "state/legacy-migration-complete").exists())

    def test_disabled_target_persists_and_original_values_remain_restorable(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        self.run_action("mic-off")
        self.assertEqual(self.values()["ADCL Input MIC1 Boost Switch"], "off")
        self.assertIn("MIC_STATE=off", self.run_action("status").stdout)
        self.assertIn("PERSIST=enabled", self.run_action("status").stdout)
        self.mixer_file.write_text(json.dumps(CONTROLS))
        self.run_action("boot")
        self.assertEqual(self.values()["MIC1 gain volume"], "0")
        self.assertEqual(self.values()["ADCL Input MIC1 Boost Switch"], "off")
        self.run_action("persist-off")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/mic-target").exists())

    def test_legacy_persistent_hook_without_target_still_enables(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        (self.app / "state/mic-target").unlink()
        self.mixer_file.write_text(json.dumps(CONTROLS))
        self.run_action("boot")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")

    def test_failed_disable_preserves_previous_persistent_state_and_baseline(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        baseline = (self.app / "state/mic-baseline").read_bytes()
        self.fail_file.write_text("disable")
        self.run_action("mic-off", success=False)
        self.assertIn("PERSIST=enabled", self.run_action("status").stdout)
        self.assertIn("MIC_STATE=on", self.run_action("status").stdout)
        self.assertEqual((self.app / "state/mic-target").read_text().strip(), "on")
        self.assertEqual((self.app / "state/mic-baseline").read_bytes(), baseline)
        self.run_action("persist-off")
        self.assertEqual(self.values(), CONTROLS)

    def test_invalid_saved_target_is_retained_and_never_executed(self):
        self.run_action("mic-on")
        self.run_action("persist-on")
        target = self.app / "state/mic-target"
        target.write_text("$(touch should-not-exist)\n")
        previous = self.log.read_bytes()
        self.run_action("boot", success=False)
        self.assertEqual(self.log.read_bytes(), previous)
        self.assertTrue(target.exists())
        self.assertFalse((self.app / "should-not-exist").exists())

    def test_diagnostic_export_is_read_only_and_includes_audio_midi(self):
        result = self.run_cli("export-log")
        path = Path(result.stdout.strip().split("=", 1)[1])
        self.assertEqual(path.parent, self.app / "diagnostics")
        report = path.read_text()
        self.assertIn("ALSA PCM devices", report)
        self.assertIn("MIDI sequencer clients", report)
        self.assertIn("Unavailable", report)
        self.assertIn("Hardware: test A133", report)
        self.assertNotIn("PRIVATE-SERIAL", report)
        self.assertIn("PCM current hardware parameters", report)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.log.exists())
        self.assertFalse((self.app / "state").exists())

    def test_usb_audio_preflight_blocks_known_smart_pro_kernel_without_mutating_state(self):
        modules = self.root / "proc/modules"
        modules.write_text("snd 123 0 - Live 0\n")
        adbd = self.root / "etc/init.d/adbd"
        adbd.parent.mkdir(parents=True, exist_ok=True)
        adbd.write_text("#!/bin/sh\n")
        result = self.run_cli("diagnostics")
        self.assertIn("USB_AUDIO_STATUS=unavailable", result.stdout)
        self.assertIn("USB_AUDIO_MODE=experimental-UAC1-IN-only", result.stdout)
        self.assertIn("USB_AUDIO_UAC2=blocked", result.stdout)
        self.assertIn("USB_AUDIO_REASON=UAC2 binding caused a kernel error on this Smart Pro.", result.stdout)
        self.assertIn("USB_AUDIO_UAC1_MODULES=missing", result.stdout)
        self.assertIn("USB_AUDIO_UAC1_FUNCTION=absent", result.stdout)
        self.assertIn("USB_AUDIO_ADB_RECOVERY=unavailable", result.stdout)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse(self.log.exists())

    def test_usb_audio_in_starts_only_on_supported_smart_pro_and_restores_adb(self):
        self.prepare_usb_gadget()
        status = self.run_action("status")
        self.assertIn("USB_AUDIO_STATE=available", status.stdout)
        started = self.run_action("usb-audio-on")
        self.assertIn("Experimental USB audio IN is active", started.stdout)
        self.assertIn("USB_AUDIO_STATE=active", self.run_action("status").stdout)
        self.assertIn("adbd:stop", (self.base / "usb-actions").read_text())
        self.assertIn("setusbconfig:uac1", (self.base / "usb-actions").read_text())
        self.assertTrue((self.app / "state").exists())
        self.run_action("usb-audio-off")
        self.assertIn("USB_AUDIO_STATE=available", self.run_action("status").stdout)
        self.assertIn("adbd:start", (self.base / "usb-actions").read_text())
        self.assertFalse((self.root / "tmp/choochoohacker-uac1").exists())

    def test_usb_audio_waits_for_host_enumeration_after_rebinding(self):
        self.prepare_usb_gadget()
        Path(self.env["FAKE_USB_DELAY_ENUMERATION"]).touch()
        started = self.run_action("usb-audio-on")
        self.assertIn("Experimental USB audio IN is active", started.stdout)
        self.assertIn("USB_AUDIO_STATE=active", self.run_action("status").stdout)

    def test_usb_audio_start_failure_restores_adb_and_keeps_no_active_marker(self):
        self.prepare_usb_gadget()
        (self.base / "fail-helper").write_text("fail")
        result = self.run_action("usb-audio-on", success=False)
        self.assertIn("UAC1 did not bind; restoring ADB.", result.stderr)
        self.assertTrue((self.root / "sys/kernel/config/usb_gadget/g1/configs/c.1/ffs.adb").is_symlink())
        self.assertIn("USB_AUDIO_STATE=available", self.run_action("status").stdout)
        self.assertFalse((self.root / "tmp/choochoohacker-uac1").exists())

    def test_usb_audio_rejects_another_trimui_model_before_loading_modules(self):
        self.prepare_usb_gadget()
        (self.root / "usr/trimui/bin/MainUI").write_text("Trimui Brick\n")
        result = self.run_action("usb-audio-on", success=False)
        self.assertIn("only available on the tested Smart Pro firmware", result.stderr)
        self.assertFalse((self.base / "usb-actions").exists())

    def test_usb_audio_keeps_recovery_marker_when_adb_restore_fails(self):
        self.prepare_usb_gadget()
        self.run_action("usb-audio-on")
        (self.base / "fail-adbd-start").write_text("fail")
        result = self.run_action("usb-audio-off", success=False)
        self.assertIn("Could not restore ADB", result.stderr)
        self.assertTrue((self.root / "tmp/choochoohacker-uac1").exists())
        (self.base / "fail-adbd-start").unlink()
        self.run_action("usb-audio-off")
        self.assertTrue((self.root / "sys/kernel/config/usb_gadget/g1/configs/c.1/ffs.adb").is_symlink())

    def test_usb_audio_cli_actions_work_without_initializing_the_display(self):
        self.prepare_usb_gadget()
        self.run_cli("usb-audio-on")
        self.run_cli("usb-audio-off")
        self.assertIn("adbd:start", (self.base / "usb-actions").read_text())

    def test_discovery_uses_named_inputs_reports_failures_and_never_changes_mixers(self):
        extra = self.root / "proc/asound/card0/pcm1c/info"
        extra.parent.mkdir()
        extra.write_text("device: 1\nname: second capture\n")
        probe = self.app / "choochoohacker.aarch64"
        probe.write_text("#!/bin/sh\n[ \"$1\" = --probe-device ] || exit 2\n"
                         "case \"$2\" in hw:CARD=audiocodec,DEV=0) echo 'stream opened'; exit 0;; *) echo 'probe failed' >&2; exit 1;; esac\n")
        probe.chmod(0o755)
        result = self.run_cli("inputs")
        self.assertIn("INPUT_1=hw:CARD=audiocodec,DEV=0", result.stdout)
        result = self.run_cli("discover")
        self.assertIn("streams opened: 1", result.stdout)
        self.assertIn("ERROR: Input probe failed", result.stdout)
        self.assertIn("DISCOVERED_INPUT=hw:CARD=audiocodec,DEV=0", result.stdout)
        self.assertEqual((self.app / "discovery.log").read_text(), result.stdout)
        self.assertFalse(self.log.exists())

    def test_unknown_hardware_input_experiments_restore_exact_values_on_cleanup(self):
        (self.root / "usr/trimui/bin/MainUI").write_text("Unknown handheld\n")
        result = self.run_cli("controls")
        self.assertIn("MIC1 gain volume|INTEGER|0|31|1|4", result.stdout)
        self.run_cli("experiment-set", "audiocodec", "2", "7")
        self.run_cli("experiment-set", "audiocodec", "2", "8")
        self.run_cli("experiment-set", "audiocodec", "3", "50,60")
        self.assertEqual(self.values()["ADC volume"], "50,60")
        self.run_action("cleanup")
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/experiments").exists())
        self.assertFalse(self.hook.exists())

    def test_experiments_reject_outputs_bad_ranges_channels_and_command_strings(self):
        state = dict(CONTROLS, **{"MIC Speaker Playback volume": "10", "Capture Source": "0"})
        self.mixer_file.write_text(json.dumps(state))
        result = self.run_cli("controls")
        self.assertNotIn("Playback", result.stdout)
        self.assertIn("Capture Source|ENUMERATED|0|2", result.stdout)
        for card, number, value in [("audiocodec", "4", "11"), ("audiocodec", "2", "32"),
                                    ("audiocodec", "3", "22"), ("audiocodec", "1", "1"),
                                    ("audiocodec", "2", "$(touch BAD)"),
                                    ("audiocodec", "5", "3"), ("bad;command", "2", "7")]:
            with self.subTest(card=card, number=number, value=value):
                self.run_cli("experiment-set", card, number, value, code=1)
        self.assertFalse(self.log.exists())
        self.run_cli("experiment-set", "audiocodec", "1", "on")
        self.run_cli("experiment-set", "audiocodec", "5", "2")
        self.run_cli("experiment-restore")
        self.assertEqual(self.values(), state)

    def test_experiment_restore_refuses_replaced_device_and_other_boot(self):
        self.run_cli("experiment-set", "audiocodec", "2", "7")
        node = self.root / "dev/snd/controlC0"
        node.rename(node.with_name("disconnected"))
        node.write_text("")
        previous = self.log.read_bytes()
        self.run_cli("experiment-restore", code=1)
        self.assertEqual(self.log.read_bytes(), previous)
        node.unlink()
        node.with_name("disconnected").rename(node)
        boot = self.root / "proc/sys/kernel/random/boot_id"
        original = boot.read_text()
        boot.write_text("1111-2222-3333-4444\n")
        self.run_cli("experiment-restore", code=1)
        self.assertEqual(self.log.read_bytes(), previous)
        boot.write_text(original)
        self.run_cli("experiment-restore")
        self.assertEqual(self.values(), CONTROLS)

    def test_failed_input_experiment_rolls_back_and_releases_lock(self):
        self.fail_file.touch()
        self.run_cli("experiment-set", "audiocodec", "2", "31", code=1)
        self.assertEqual(self.values(), CONTROLS)
        self.assertFalse((self.app / "state/experiments/audiocodec.2").exists())
        self.assertFalse((self.app / "state/lock").exists())

    def test_foreign_hook_is_untouched(self):
        self.hook.parent.mkdir(parents=True)
        self.hook.write_text("# another application's hook\n")
        self.run_action("mic-on", success=False)
        self.run_action("persist-on", success=False)
        self.run_action("persist-off", success=False)
        self.run_action("setup-default", success=False)
        self.assertEqual(self.hook.read_text(), "# another application's hook\n")
        self.assertFalse(self.log.exists())

    def test_unknown_hardware_and_unsupported_nextui_profile_are_read_only(self):
        model = self.root / "usr/trimui/bin/MainUI"
        model.write_text("Unknown handheld\n")
        self.run_action("mic-on", success=False)
        self.run_action("cleanup")
        (self.root / "mnt/SDCARD/.system/tg5040/bin/run_hooks.sh").unlink()
        self.run_action("setup-default", success=False)
        self.assertFalse(self.log.exists())

    def test_tg5040_brick_profile_is_supported(self):
        model = self.root / "usr/trimui/bin/MainUI"
        model.write_text("Trimui Brick\n")
        self.run_action("setup-default")
        self.assertEqual(self.values()["MIC1 gain volume"], "31")

    def test_corrupt_baseline_is_kept_and_not_executed(self):
        state = self.app / "state"
        state.mkdir()
        baseline = state / "mic-baseline"
        baseline.write_text("on\n$(touch should-not-exist)\n160,160\n")
        self.run_action("restore", success=False)
        self.assertTrue(baseline.exists())
        self.assertFalse(self.log.exists())
        self.assertFalse((self.app / "should-not-exist").exists())

    def test_concurrent_action_is_rejected(self):
        (self.app / "state/lock").mkdir(parents=True)
        self.run_action("mic-on", success=False)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
