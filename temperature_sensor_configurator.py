import tkinter as tk
from tkinter import Toplevel, ttk, filedialog, messagebox, PhotoImage
import threading
import subprocess
import os
import serial
import serial.tools.list_ports
import resources
from PIL import Image, ImageTk
import sys
import shutil
import tempfile
import logging
import re

logging.basicConfig(filename='configurator.log', level=logging.DEBUG,
                    format='%(asctime)s - %(levelname)s - %(message)s')

class TemperatureSensorConfigurator:
    def __init__(self, root):
        self.window = Toplevel(root)
        self.window.title("Конфигурация датчика температуры")
        self.window.iconphoto(False, PhotoImage(file=resources.get_icon_path()))

        self.window.resizable(True, True)
        self.window.wm_minsize(800, 700)

        self.window.grid_rowconfigure(2, weight=1)
        self.window.grid_columnconfigure(0, weight=1, minsize=400)
        self.window.grid_columnconfigure(1, weight=1)
        self.window.grid_columnconfigure(2, weight=0)

        # --- Image Frame ---
        image_frame = ttk.Frame(self.window, padding=0, borderwidth=2, relief="solid")
        image_frame.grid(column=0, row=0, sticky='nsew', padx=5, pady=5)

        image_path = resources.get_image_path('temp.png')
        self.image = self.load_image(image_path, (100, 100))
        self.image_label = tk.Label(image_frame, image=self.image)
        self.image_label.pack(pady=5)

        self.tooltip_label = ttk.Label(image_frame, text="?", font=("Arial", 12, "bold"), foreground="blue",
                                       cursor="hand2")
        self.tooltip_label.place(relx=1.0, rely=1.0, anchor='se', x=-5, y=-5)
        self.tooltip_label.bind("<Enter>",
                                lambda e: self.show_tooltip(e, "Настройте параметры для датчика температуры (Проводной/Беспроводной)."))
        self.tooltip_label.bind("<Leave>", self.hide_tooltip)

        # --- Port Frame ---
        port_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=0)
        port_frame.grid(column=0, row=1, sticky='nsew', padx=5, pady=5)
        port_frame.grid_columnconfigure(1, weight=1)

        self.port_label = ttk.Label(port_frame, text="Выберите COM-порт:")
        self.port_label.grid(column=0, row=0, padx=5, pady=10)

        self.port_combobox = ttk.Combobox(port_frame, state="readonly", width=10)
        self.port_combobox.grid(column=1, row=0, padx=10, pady=10, sticky="ew")

        self.refresh_button = ttk.Button(port_frame, text="Обновить", command=self.refresh_ports)
        self.refresh_button.grid(column=2, row=0, padx=5, pady=10)

        self.connect_button = ttk.Button(port_frame, text="Подключить", command=self.connect_device)
        self.connect_button.grid(column=3, row=0, padx=5, pady=10)

        # --- Notebook for Tabs ---
        self.notebook = ttk.Notebook(self.window)
        self.notebook.grid(column=0, row=2, sticky='nsew', padx=5, pady=5)

        self.wired_frame = ttk.Frame(self.notebook, padding=10)
        self.wireless_frame = ttk.Frame(self.notebook, padding=10)

        self.notebook.add(self.wired_frame, text="Проводной")
        self.notebook.add(self.wireless_frame, text="Беспроводной")

        # --- Wired Tab Content ---
        self.setup_wired_tab()

        # --- Wireless Tab Content ---
        self.setup_wireless_tab()

        # --- Response & Diagnostics ---
        response_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        response_frame.grid(column=1, row=0, rowspan=3, sticky='nsew', padx=5, pady=5)

        self.response_text = tk.Text(response_frame, height=30, width=30)
        self.response_text.pack(fill=tk.BOTH, expand=True)
        self.response_text.bind("<Control-c>", self.copy_text)
        self.create_context_menu()

        diagnostics_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        diagnostics_frame.grid(column=2, row=0, rowspan=3, sticky='nsew', padx=5, pady=5)

        self.diagnostics_var = tk.BooleanVar(value=True)
        self.diagnostics_checkbutton = ttk.Checkbutton(diagnostics_frame, text="Диагностика", variable=self.diagnostics_var, command=self.toggle_diagnostics)
        self.diagnostics_checkbutton.pack(padx=10, pady=10)

        self.ports = []
        self.serial_connection = None
        self.reading_enabled = True
        self.tooltip_window = None

        self.refresh_ports()

    def setup_wired_tab(self):
        # Firmware
        firmware_frame = ttk.LabelFrame(self.wired_frame, text="Прошивка", padding=5)
        firmware_frame.pack(fill='x', pady=5)
        
        self.wired_firmware_path = tk.StringVar(value=self.get_firmware_path('esp32_temp.bin') or "")
        ttk.Entry(firmware_frame, textvariable=self.wired_firmware_path).pack(side='left', fill='x', expand=True, padx=5)
        ttk.Button(firmware_frame, text="Обзор", command=lambda: self.select_firmware_file(self.wired_firmware_path)).pack(side='left', padx=5)
        ttk.Button(firmware_frame, text="Прошить", command=lambda: self.flash_esp32(self.wired_firmware_path, self.wired_progress, self.wired_percent)).pack(side='left', padx=5)
        
        progress_frame = ttk.Frame(firmware_frame)
        progress_frame.pack(fill='x', pady=5)
        self.wired_progress = ttk.Progressbar(progress_frame, mode='determinate')
        self.wired_progress.pack(side='left', fill='x', expand=True)
        self.wired_percent = ttk.Label(progress_frame, text="0%")
        self.wired_percent.pack(side='left', padx=5)

        # Settings
        settings_frame = ttk.LabelFrame(self.wired_frame, text="Настройки", padding=5)
        settings_frame.pack(fill='both', expand=True, pady=5)

        wired_tooltip_label = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"), foreground="blue", cursor="hand2")
        wired_tooltip_label.place(relx=1.0, rely=0, anchor='ne', x=-5, y=5)
        wired_tooltip_text = "Датчик геркона: пин 27\nДатчик температуры (DS18B20): пин 32\nURL по умолчанию: http://80.80.101.123:503/data"
        wired_tooltip_label.bind("<Enter>", lambda e: self.show_tooltip(e, wired_tooltip_text))
        wired_tooltip_label.bind("<Leave>", self.hide_tooltip)

        self.wired_device_id = self.create_setting_row(settings_frame, "Device ID:", 0, self.set_device_id)
        self.wired_client_id = self.create_setting_row(settings_frame, "Client ID:", 1, self.set_client_id)
        self.wired_ssid = self.create_setting_row(settings_frame, "SSID:", 2, self.set_ssid)
        self.wired_password = self.create_setting_row(settings_frame, "Password:", 3, self.set_password, show="*")
        self.wired_url = self.create_setting_row(settings_frame, "URL:", 4, self.set_url)

        ttk.Button(settings_frame, text="Считать настройки", command=self.get_settings).grid(column=0, row=5, columnspan=3, pady=10, sticky='ew')

    def setup_wireless_tab(self):
        # Firmware
        firmware_frame = ttk.LabelFrame(self.wireless_frame, text="Прошивка", padding=5)
        firmware_frame.pack(fill='x', pady=5)
        
        self.wireless_firmware_path = tk.StringVar(value=self.get_firmware_path('esp32_temp_ble.bin') or "")
        ttk.Entry(firmware_frame, textvariable=self.wireless_firmware_path).pack(side='left', fill='x', expand=True, padx=5)
        ttk.Button(firmware_frame, text="Обзор", command=lambda: self.select_firmware_file(self.wireless_firmware_path)).pack(side='left', padx=5)
        ttk.Button(firmware_frame, text="Прошить", command=lambda: self.flash_esp32(self.wireless_firmware_path, self.wireless_progress, self.wireless_percent)).pack(side='left', padx=5)
        
        progress_frame = ttk.Frame(firmware_frame)
        progress_frame.pack(fill='x', pady=5)
        self.wireless_progress = ttk.Progressbar(progress_frame, mode='determinate')
        self.wireless_progress.pack(side='left', fill='x', expand=True)
        self.wireless_percent = ttk.Label(progress_frame, text="0%")
        self.wireless_percent.pack(side='left', padx=5)

        # Settings
        settings_frame = ttk.LabelFrame(self.wireless_frame, text="Настройки", padding=5)
        settings_frame.pack(fill='both', expand=True, pady=5)

        wireless_tooltip_label = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"), foreground="blue", cursor="hand2")
        wireless_tooltip_label.place(relx=1.0, rely=0, anchor='ne', x=-5, y=5)
        wireless_tooltip_text = "Датчик геркона: пин 27"
        wireless_tooltip_label.bind("<Enter>", lambda e: self.show_tooltip(e, wireless_tooltip_text))
        wireless_tooltip_label.bind("<Leave>", self.hide_tooltip)

        self.wireless_device_id = self.create_setting_row(settings_frame, "Device ID:", 0, self.set_device_id)
        self.wireless_client_id = self.create_setting_row(settings_frame, "Client ID:", 1, self.set_client_id)
        self.wireless_ssid = self.create_setting_row(settings_frame, "SSID:", 2, self.set_ssid)
        self.wireless_password = self.create_setting_row(settings_frame, "Password:", 3, self.set_password, show="*")
        self.wireless_ble_mac = self.create_setting_row(settings_frame, "BLE MAC:", 4, self.set_ble_mac)

        ttk.Button(settings_frame, text="Считать настройки", command=self.get_settings).grid(column=0, row=5, columnspan=3, pady=10, sticky='ew')

    def create_setting_row(self, parent, label_text, row, command_func, show=None):
        ttk.Label(parent, text=label_text).grid(column=0, row=row, padx=5, pady=5, sticky='e')
        entry = ttk.Entry(parent, width=25, show=show)
        entry.grid(column=1, row=row, padx=5, pady=5, sticky='ew')
        ttk.Button(parent, text="Установить", command=lambda: command_func(entry.get())).grid(column=2, row=row, padx=5, pady=5)
        return entry

    def flash_esp32(self, firmware_path_var, progress_bar, percent_label):
        port = self.port_combobox.get()
        firmware_file = firmware_path_var.get()

        if not port:
            messagebox.showerror("Ошибка", "Выберите COM-порт для прошивки.")
            return
        if not firmware_file or not os.path.exists(firmware_file):
            messagebox.showerror("Ошибка", f"Файл прошивки не найден: {firmware_file}.")
            return

        temp_dir = tempfile.gettempdir()
        temp_firmware_path = os.path.join(temp_dir, os.path.basename(firmware_file))
        try:
            shutil.copy(firmware_file, temp_firmware_path)
        except Exception as e:
            messagebox.showerror("Ошибка", f"Не удалось скопировать файл прошивки: {str(e)}")
            return

        def flash():
            class RealTimeOutput:
                def __init__(self, window, p_bar, p_label, out_stream):
                    self.window = window
                    self.p_bar = p_bar
                    self.p_label = p_label
                    self.out_stream = out_stream
                    self.buffer = ""
                    self.full_log = []

                def write(self, string):
                    # Дублируем вывод в консоль PyCharm
                    if self.out_stream:
                        self.out_stream.write(string)
                        self.out_stream.flush()

                    if not string: return
                    self.full_log.append(string)
                    self.buffer += string
                    
                    # Оставляем последние 100 символов для анализа
                    if len(self.buffer) > 100:
                        self.buffer = self.buffer[-100:]

                    # Ищем проценты в буфере. 
                    # Паттерн: число (возможно с точкой), за которым следует %
                    # Примеры: 10%, 10.5%, 10 %, (10 %)
                    matches = re.findall(r'(\d{1,3})(?:\.\d+)?\s?%', self.buffer)
                    if matches:
                        try:
                            percent = int(matches[-1])
                            self.window.after(0, self.update_ui, percent)
                        except ValueError:
                            pass

                def update_ui(self, percent):
                    self.p_bar['value'] = percent
                    self.p_label['text'] = f"{percent}%"
                    self.window.update_idletasks()

                def flush(self):
                    if self.out_stream:
                        self.out_stream.flush()
                
                def isatty(self):
                    return True
                
                def getvalue(self):
                    return "".join(self.full_log)

            # Сохраняем оригинальные stdout/stderr
            original_stdout = sys.stdout
            original_stderr = sys.stderr
            
            # Создаем перехватчик, передавая ему оригинальный stdout для вывода в консоль
            real_time_out = RealTimeOutput(self.window, progress_bar, percent_label, original_stdout)
            
            sys.stdout = real_time_out
            sys.stderr = real_time_out

            try:
                if getattr(sys, 'frozen', False):
                    esptool_path = os.path.join(sys._MEIPASS, 'run_esptool.py')
                else:
                    esptool_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'run_esptool.py')

                args = [
                    "--chip", "esp32", "--port", port, "--baud", "921600", "--before", "default_reset",
                    "--after", "hard_reset", "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
                    "--flash_size", "4MB", "0x10000", temp_firmware_path
                ]
                
                import run_esptool
                sys.argv = ['esptool'] + args
                success = False

                try:
                    run_esptool.main()
                    success = True
                except SystemExit: pass
                except Exception as e: logging.error(f"Исключение в run_esptool: {str(e)}")

                output_str = real_time_out.getvalue()
                
                # Восстанавливаем вывод
                sys.stdout = original_stdout
                sys.stderr = original_stderr

                if success and "Hash of data verified" in output_str:
                    self.window.after(0, lambda: progress_bar.configure(value=100))
                    self.window.after(0, lambda: percent_label.configure(text="100%"))
                    messagebox.showinfo("Успех", "Прошивка завершена успешно!")
                else:
                    messagebox.showerror("Ошибка", f"Прошивка завершилась с ошибкой:\n{output_str or 'Нет вывода от esptool'}")
            
            except Exception as e:
                sys.stdout = original_stdout
                sys.stderr = original_stderr
                messagebox.showerror("Ошибка", f"Произошла ошибка: {str(e)}")
            finally:
                sys.stdout = original_stdout
                sys.stderr = original_stderr
                if os.path.exists(temp_firmware_path):
                    os.remove(temp_firmware_path)

        threading.Thread(target=flash, daemon=True).start()

    def get_firmware_path(self, firmware_name):
        try:
            if getattr(sys, 'frozen', False):
                base_path = sys._MEIPASS
            else:
                base_path = os.path.dirname(os.path.abspath(__file__))
            firmware_path = os.path.join(base_path, firmware_name)
            if not os.path.exists(firmware_path): return None
            return firmware_path
        except Exception:
            return None

    def select_firmware_file(self, path_var):
        file_path = filedialog.askopenfilename(filetypes=(("Bin файлы", "*.bin"), ("Все файлы", "*.*")))
        if file_path:
            path_var.set(file_path)

    def create_context_menu(self):
        self.context_menu = tk.Menu(self.window, tearoff=0)
        self.context_menu.add_command(label="Копировать", command=self.copy_text)
        self.response_text.bind("<Button-3>", self.show_context_menu)

    def show_context_menu(self, event):
        self.context_menu.post(event.x_root, event.y_root)

    def copy_text(self, event=None):
        try:
            self.response_text.clipboard_clear()
            self.response_text.clipboard_append(self.response_text.selection_get())
        except tk.TclError: pass

    def load_image(self, path, size):
        try:
            return ImageTk.PhotoImage(Image.open(path).resize(size))
        except Exception: return None

    def refresh_ports(self):
        self.ports = serial.tools.list_ports.comports()
        self.port_combobox['values'] = [port.device for port in self.ports]

    def connect_device(self):
        selected_port = self.port_combobox.get()
        if selected_port:
            try:
                self.serial_connection = serial.Serial(selected_port, 9600, timeout=0.1)
                self.response_text.insert(tk.END, f"Подключено к {selected_port}\n")
                self.window.after(100, self.read_serial_data)
            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка подключения: {str(e)}\n")

    def send_command(self, command, value):
        if not value:
             messagebox.showerror("Ошибка", "Значение не может быть пустым.")
             return
        full_command = f"{command} {value}\n"
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.write(full_command.encode('utf-8'))
            self.response_text.insert(tk.END, f"Отправлена команда: {full_command.strip()}\n")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")

    def set_device_id(self, device_id):
        if device_id.strip().isdigit(): self.send_command("setdeviceid", device_id.strip())
        else: messagebox.showerror("Ошибка", "Неправильный формат Device ID. Введите число.")

    def set_client_id(self, client_id): self.send_command("setclientid", client_id.strip())
    def set_ssid(self, ssid): self.send_command("setssid", ssid.strip())
    def set_password(self, password): self.send_command("setpassword", password.strip())

    def set_ble_mac(self, ble_mac):
        ble_mac = ble_mac.strip()
        if ble_mac and len(ble_mac) == 17 and ':' in ble_mac: self.send_command("setblemac", ble_mac)
        else: messagebox.showerror("Ошибка", "Введите корректный MAC адрес (формат: AA:BB:CC:DD:EE:FF)")

    def set_url(self, url):
        url = url.strip()
        if url and url.startswith("http://"): self.send_command("seturl", url)
        else: messagebox.showerror("Ошибка", "Введите корректный URL (например, http://80.80.101.123:503/data).")

    def get_settings(self):
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.write("getsettings\n".encode('utf-8'))
            self.response_text.insert(tk.END, "Запрос настроек отправлен.\n")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")

    def read_serial_data(self):
        if self.serial_connection and self.serial_connection.is_open and self.reading_enabled:
            try:
                if self.serial_connection.in_waiting > 0:
                    response = self.serial_connection.read(self.serial_connection.in_waiting).decode('utf-8', errors='ignore').strip()
                    if response:
                        self.response_text.insert(tk.END, f"Получено: {response}\n")
                        self.response_text.see(tk.END)
                self.window.after(50, self.read_serial_data)
            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка чтения: {str(e)}\n")
                if self.serial_connection.is_open: self.serial_connection.close()
                self.serial_connection = None

    def toggle_diagnostics(self):
        if self.serial_connection and self.serial_connection.is_open:
            if self.diagnostics_var.get():
                self.reading_enabled = True
                self.response_text.insert(tk.END, "Диагностика включена.\n")
                self.window.after(100, self.read_serial_data)
            else:
                self.reading_enabled = False
                self.response_text.insert(tk.END, "Диагностика выключена.\n")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")

    def show_tooltip(self, event, text):
        try:
            self.tooltip_window = tk.Toplevel(self.window)
            self.tooltip_window.wm_overrideredirect(True)
            self.tooltip_window.wm_geometry(f"+{event.x_root + 10}+{event.y_root + 10}")
            tk.Label(self.tooltip_window, text=text, background="yellow", relief="solid", borderwidth=1, padx=5, pady=3, font=("Arial", 10, "bold"), justify='left').pack()
        except Exception: pass

    def hide_tooltip(self, event):
        if self.tooltip_window:
            self.tooltip_window.destroy()
            self.tooltip_window = None

if __name__ == "__main__":
    root = tk.Tk()
    app = TemperatureSensorConfigurator(root)
    root.mainloop()