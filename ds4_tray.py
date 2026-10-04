#!/usr/bin/env python3
# ds4_tray.py — DS4 Bridge (AppIndicator / Tray version with Log management)
import os
import subprocess
import gi

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

try:
    gi.require_version('AyatanaAppIndicator3', '0.1')
    from gi.repository import AyatanaAppIndicator3 as AppIndicator
except ValueError:
    try:
        gi.require_version('AppIndicator3', '0.1')
        from gi.repository import AppIndicator3 as AppIndicator
    except ValueError:
        print("ERROR: AppIndicator library not found to create tray icon.")
        exit(1)

APP_INDICATOR_ID = 'ds4-bridge-tray'

class DS4TrayApp:
    def __init__(self):
        self.script_dir = os.path.dirname(os.path.realpath(__file__))
        self.executable = os.path.join(self.script_dir, "ds4_bridge")
        # Artik loglar sistem journal-ina yox, bu qovluqdaki fayla yazilacaq
        self.log_file_path = os.path.join(self.script_dir, "ds4_bridge.log")
        
        self.indicator = AppIndicator.Indicator.new(
            APP_INDICATOR_ID,
            "input-gaming",
            AppIndicator.IndicatorCategory.APPLICATION_STATUS)
        self.indicator.set_status(AppIndicator.IndicatorStatus.ACTIVE)
        
        self.bridge_process = None
        self.logs_enabled = True
        
        self.menu = Gtk.Menu()
        
        self.status_item = Gtk.MenuItem(label="Status: Checking...")
        self.status_item.set_sensitive(False)
        self.menu.append(self.status_item)
        
        self.menu.append(Gtk.SeparatorMenuItem())
        
        self.start_item = Gtk.MenuItem(label="▶ Start Bridge")
        self.start_item.connect('activate', self.start_bridge)
        self.menu.append(self.start_item)
        
        self.stop_item = Gtk.MenuItem(label="⏹ Stop Bridge")
        self.stop_item.connect('activate', self.stop_bridge)
        self.menu.append(self.stop_item)
        
        self.menu.append(Gtk.SeparatorMenuItem())
        
        # Loglari acib/baglamaq ucun secim
        self.log_toggle = Gtk.CheckMenuItem(label="📝 Enable Logs (Write logs)")
        self.log_toggle.set_active(True)
        self.log_toggle.connect('toggled', self.toggle_logs)
        self.menu.append(self.log_toggle)

        self.view_log_item = Gtk.MenuItem(label="📄 View Logs")
        self.view_log_item.connect('activate', self.show_logs)
        self.menu.append(self.view_log_item)
        
        self.menu.append(Gtk.SeparatorMenuItem())
        
        quit_item = Gtk.MenuItem(label="❌ Quit")
        quit_item.connect('activate', self.quit_prompt)
        self.menu.append(quit_item)
        
        self.menu.show_all()
        self.indicator.set_menu(self.menu)
        
        self.update_status()
        GLib.timeout_add_seconds(2, self.update_status)
        
    def toggle_logs(self, widget):
        self.logs_enabled = widget.get_active()
        # Eger proqram isleyirse, log deyisikliyini tetbiq etmek ucun restart edirik
        if self.bridge_process and self.bridge_process.poll() is None:
            self.stop_bridge(None)
            self.start_bridge(None)

    def check_process(self):
        if self.bridge_process:
            return self.bridge_process.poll() is None
        try:
            subprocess.check_output(["pgrep", "-x", "ds4_bridge"])
            return True
        except subprocess.CalledProcessError:
            return False

    def update_status(self):
        running = self.check_process()
        if running:
            self.status_item.set_label("Status: Running 🟢")
            self.indicator.set_icon_full("input-gaming", "Running")
            self.start_item.set_sensitive(False)
            self.stop_item.set_sensitive(True)
        else:
            self.status_item.set_label("Status: Stopped 🔴")
            self.indicator.set_icon_full("media-playback-pause", "Stopped")
            self.start_item.set_sensitive(True)
            self.stop_item.set_sensitive(False)
        return True
        
    def start_bridge(self, _):
        if not os.path.exists(self.executable):
            return
            
        os.system("pkill -x ds4_bridge")
        
        # Log fayli ve ya Hecne (/dev/null)
        if self.logs_enabled:
            out_file = open(self.log_file_path, "a")
        else:
            out_file = subprocess.DEVNULL
            
        # Proqrami on planda (daemon olmadan) isledib output-u fayla yonlendiririk
        self.bridge_process = subprocess.Popen(
            [self.executable], 
            cwd=self.script_dir,
            stdout=out_file,
            stderr=out_file
        )
        self.update_status()

    def stop_bridge(self, _):
        if self.bridge_process:
            self.bridge_process.terminate()
            self.bridge_process.wait()
            self.bridge_process = None
        else:
            os.system("pkill -x ds4_bridge")
        self.update_status()

    def show_logs(self, _):
        logs = ""
        if os.path.exists(self.log_file_path):
            with open(self.log_file_path, "r") as f:
                lines = f.readlines()
                logs = "".join(lines[-50:])
        
        if not logs.strip():
            logs = "Logs are empty or disabled."
            
        win = Gtk.Window(title="DS4 Bridge Logs")
        win.set_default_size(600, 400)
        win.set_position(Gtk.WindowPosition.CENTER)
        
        scrolled = Gtk.ScrolledWindow()
        textview = Gtk.TextView()
        textview.set_editable(False)
        textview.get_buffer().set_text(logs)
        textview.override_font(gi.repository.Pango.FontDescription('Monospace 10'))
        
        adj = scrolled.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper() - adj.get_page_size()))

        scrolled.add(textview)
        win.add(scrolled)
        win.show_all()

    def quit_prompt(self, _):
        # Cixis ederken loglarin silinmesi barede sorusur
        if os.path.exists(self.log_file_path):
            dialog = Gtk.MessageDialog(
                transient_for=None,
                flags=0,
                message_type=Gtk.MessageType.QUESTION,
                buttons=Gtk.ButtonsType.YES_NO,
                text="Closing DS4 Bridge"
            )
            dialog.format_secondary_text("Do you want to delete the log file before quitting?\n(This keeps your PC clean)")
            response = dialog.run()
            dialog.destroy()
            
            if response == Gtk.ResponseType.YES:
                try:
                    os.remove(self.log_file_path)
                except Exception:
                    pass
                
        self.quit(None)

    def quit(self, _):
        self.stop_bridge(None)
        Gtk.main_quit()

if __name__ == "__main__":
    app = DS4TrayApp()
    Gtk.main()
