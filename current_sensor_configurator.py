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

logging.basicConfig(filename='configurator.log', level=logging.DEBUG,
                    format='%(asctime)s - %(levelname)s - %(message)s')

class CurrentSensorConfigurator:
    def __init__(self, root):
        self.window = Toplevel(root)
        self.window.title("Конфигурация Датчика тока")
        self.window.iconphoto(False, PhotoImage(file=resources.get_icon_path()))

        self.window.resizable(True, True)
        self.window.wm_minsize(650, 650)

        self.window.grid_rowconfigure(0, weight=1)
        self.window.grid_columnconfigure(0, weight=1, minsize=300)
        self.window.grid_columnconfigure(1, weight=1)
        self.window.grid_columnconfigure(2, weight=1)

        image_frame = ttk.Frame(self.window, padding=0, borderwidth=2, relief="solid")
        image_frame.grid(column=0, row=0, columnspan=1, sticky='nsew', padx=5, pady=5)

        image_path = resources.get_image_path('current.png')
        self.image = self.load_image(image_path, (100, 100))
        self.image_label = tk.Label(image_frame, image=self.image)
        self.image_label.pack(pady=5)

        self.tooltip_label = ttk.Label(image_frame, text="?", font=("Arial", 12, "bold"), foreground="blue",
                                       cursor="hand2")
        self.tooltip_label.place(relx=1.0, rely=1.0, anchor='se', x=-5, y=-5)
        self.tooltip_label.bind("<Enter>",
                                lambda e: self.show_tooltip(e, "Настройте параметры для датчика тока."))
        self.tooltip_label.bind("<Leave>", self.hide_tooltip)

        port_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=0)
        port_frame.grid(column=0, row=1, columnspan=1, sticky='nsew', padx=5, pady=5)
        port_frame.grid_columnconfigure(1, weight=1)
        port_frame.grid_columnconfigure(3, weight=1)
        port_frame.grid_columnconfigure(4, weight=1)

        self.port_label = ttk.Label(port_frame, text="Выберите COM-порт:")
        self.port_label.grid(column=0, row=0, padx=5, pady=10)

        self.port_combobox = ttk.Combobox(port_frame, state="readonly", width=10)
        self.port_combobox.grid(column=1, row=0, columnspan=2, padx=10, pady=10, sticky="ew")

        self.refresh_button = ttk.Button(port_frame, text="Обновить", command=self.refresh_ports)
        self.refresh_button.grid(column=3, row=0, padx=5, pady=10, sticky="ew")

        self.connect_button = ttk.Button(port_frame, text="Подключить", command=self.connect_device)
        self.connect_button.grid(column=4, row=0, padx=5, pady=10, sticky="ew")

        firmware_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=5)
        firmware_frame.grid(column=0, row=2, columnspan=1, sticky='nsew', padx=5, pady=5)
        firmware_frame.grid_columnconfigure(1, weight=1)

        firmware_label = ttk.Label(firmware_frame, text="Выберите файл прошивки:")
        firmware_label.grid(column=0, row=0, padx=0, pady=0)

        self.firmware_path = tk.StringVar(value=self.get_firmware_path('esp32_current.bin'))
        firmware_entry = ttk.Entry(firmware_frame, textvariable=self.firmware_path, width=40)
        firmware_entry.grid(column=1, row=0, padx=5, pady=10, sticky='ew')

        firmware_button = ttk.Button(firmware_frame, text="Обзор", command=self.select_firmware_file)
        firmware_button.grid(column=2, row=0, padx=5, pady=10, sticky='ew')

        flash_button = ttk.Button(firmware_frame, text="Прошить ESP32", command=self.flash_esp32)
        flash_button.grid(column=3, row=0, padx=5, pady=10, sticky='ew')

        self.progress_bar = ttk.Progressbar(firmware_frame, mode='determinate')
        self.progress_bar.grid(column=0, row=1, columnspan=4, padx=10, pady=10, sticky='ew')

        self.percent_label = ttk.Label(firmware_frame, text="0%")
        self.percent_label.grid(column=4, row=1, columnspan=1, padx=0, pady=0, sticky='ew')

        settings_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        settings_frame.grid(column=0, row=3, columnspan=5, sticky='nsew', padx=5, pady=5)

        self.device_id_label = ttk.Label(settings_frame, text="Device ID:")
        self.device_id_label.grid(column=0, row=0, padx=10, pady=10)
        self.device_id_entry = ttk.Entry(settings_frame, width=10)
        self.device_id_entry.grid(column=1, row=0, padx=10, pady=10)
        self.set_device_id_button = ttk.Button(settings_frame, text="Установить", command=self.set_device_id)
        self.set_device_id_button.grid(column=2, row=0, padx=10, pady=10)

        self.client_id_label = ttk.Label(settings_frame, text="Client ID:")
        self.client_id_label.grid(column=0, row=1, padx=10, pady=10)
        self.client_id_entry = ttk.Entry(settings_frame, width=10)
        self.client_id_entry.grid(column=1, row=1, padx=10, pady=10)
        self.set_client_id_button = ttk.Button(settings_frame, text="Установить", command=self.set_client_id)
        self.set_client_id_button.grid(column=2, row=1, padx=10, pady=10)

        self.ssid_label = ttk.Label(settings_frame, text="SSID:")
        self.ssid_label.grid(column=0, row=2, padx=10, pady=10)
        self.ssid_entry = ttk.Entry(settings_frame, width=20)
        self.ssid_entry.grid(column=1, row=2, padx=10, pady=10)
        self.set_ssid_button = ttk.Button(settings_frame, text="Установить", command=self.set_ssid)
        self.set_ssid_button.grid(column=2, row=2, padx=10, pady=10)

        self.password_label = ttk.Label(settings_frame, text="Password:")
        self.password_label.grid(column=0, row=3, padx=10, pady=10)
        self.password_entry = ttk.Entry(settings_frame, width=20, show="*")
        self.password_entry.grid(column=1, row=3, padx=10, pady=10)
        self.set_password_button = ttk.Button(settings_frame, text="Установить", command=self.set_password)
        self.set_password_button.grid(column=2, row=3, padx=10, pady=10)

        # Добавление поля для IP-адреса (URL)
        self.url_label = ttk.Label(settings_frame, text="Адрес сервера:")
        self.url_label.grid(column=0, row=4, padx=10, pady=10)
        self.url_entry = ttk.Entry(settings_frame, width=20)
        self.url_entry.grid(column=1, row=4, padx=10, pady=10)
        self.set_url_button = ttk.Button(settings_frame, text="Установить", command=self.set_url)
        self.set_url_button.grid(column=2, row=4, padx=10, pady=10)

        self.get_settings_button = ttk.Button(settings_frame, text="Считать настройки", command=self.get_settings)
        self.get_settings_button.grid(column=0, row=5, columnspan=3, padx=10, pady=10, sticky='ew')

        # Фрейм для диагностики
        diagnostics_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        diagnostics_frame.grid(column=2, row=0, rowspan=8, sticky='nsew', padx=5, pady=5)

        self.diagnostics_var = tk.BooleanVar(value=True)  # Диагностика включена по умолчанию
        self.diagnostics_checkbutton = ttk.Checkbutton(diagnostics_frame, text="Диагностика", variable=self.diagnostics_var, command=self.toggle_diagnostics)
        self.diagnostics_checkbutton.pack(padx=10, pady=10)

        response_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        response_frame.grid(column=1, row=0, rowspan=8, sticky='nsew', padx=5, pady=5)

        self.response_text = tk.Text(response_frame, height=30, width=25)
        self.response_text.pack(fill=tk.BOTH, expand=True)
        self.response_text.bind("<Control-c>", self.copy_text)
        self.create_context_menu()

        self.ports = []
        self.serial_connection = None
        self.reading_enabled = True  # Флаг для управления выводом монитора порта

        self.refresh_ports()

    def flash_esp32(self):
        port = self.port_combobox.get()
        firmware_file = self.firmware_path.get() or self.get_firmware_path('esp32_current.bin')

        if not port:
            messagebox.showerror("Ошибка", "Выберите COM-порт для прошивки.")
            logging.error("COM-порт не выбран")
            return
        if not firmware_file or not os.path.exists(firmware_file):
            messagebox.showerror("Ошибка", f"Файл прошивки не найден: {firmware_file}.")
            logging.error(f"Файл прошивки не существует: {firmware_file}")
            return
        if not os.access(firmware_file, os.R_OK):
            messagebox.showerror("Ошибка", f"Файл прошивки недоступен для чтения: {firmware_file}.")
            logging.error(f"Файл прошивки недоступен: {firmware_file}")
            return

        temp_dir = tempfile.gettempdir()
        temp_firmware_path = os.path.join(temp_dir, os.path.basename(firmware_file))
        try:
            shutil.copy(firmware_file, temp_firmware_path)
            logging.debug(f"Файл прошивки скопирован во временную папку: {temp_firmware_path}")
        except Exception as e:
            logging.error(f"Ошибка копирования файла прошивки: {str(e)}")
            messagebox.showerror("Ошибка", f"Не удалось скопировать файл прошивки: {str(e)}")
            return

        def flash():
            try:
                if getattr(sys, 'frozen', False):
                    esptool_path = os.path.join(sys._MEIPASS, 'run_esptool.py')
                else:
                    esptool_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'run_esptool.py')

                logging.debug(f"Путь к run_esptool.py: {esptool_path}")
                if not os.path.exists(esptool_path):
                    logging.error(f"Файл run_esptool.py не найден: {esptool_path}")
                    messagebox.showerror("Ошибка", f"Файл run_esptool.py не найден: {esptool_path}")
                    return

                args = [
                    "--chip", "esp32", "--port", port, "--baud", "921600", "--before", "default_reset",
                    "--after", "hard_reset", "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
                    "--flash_size", "4MB", "0x10000", temp_firmware_path
                ]
                logging.debug(f"Аргументы esptool: {args}")

                import run_esptool
                sys.argv = ['esptool'] + args
                output = []
                success = False

                import io
                from contextlib import redirect_stdout, redirect_stderr
                stdout_capture = io.StringIO()
                stderr_capture = io.StringIO()
                with redirect_stdout(stdout_capture), redirect_stderr(stderr_capture):
                    try:
                        run_esptool.main()
                        success = True
                    except SystemExit:
                        pass
                    except Exception as e:
                        logging.error(f"Исключение в run_esptool: {str(e)}")
                        output.append(f"Исключение в esptool: {str(e)}")

                stdout = stdout_capture.getvalue()
                stderr = stderr_capture.getvalue()
                output.extend([stdout, stderr])
                output_str = '\n'.join(filter(None, output))
                logging.debug(f"esptool stdout: {stdout}")
                logging.debug(f"esptool stderr: {stderr}")

                if success and "Hash of data verified" in stdout:
                    for line in stdout.splitlines():
                        if '(' in line and '%' in line:
                            percent = line.strip().split('(')[-1].split(' ')[0]
                            if percent.isdigit():
                                self.percent_label['text'] = f"{percent}%"
                                self.progress_bar['value'] = int(percent)
                                self.window.update_idletasks()
                    messagebox.showinfo("Успех", "Прошивка завершена успешно!")
                    logging.debug("Прошивка успешно завершена")
                else:
                    logging.error(f"Ошибка прошивки:\nstdout: {stdout}\nstderr: {stderr}")
                    messagebox.showerror("Ошибка", f"Прошивка завершилась с ошибкой:\n{output_str or 'Нет вывода от esptool'}")
            except Exception as e:
                logging.error(f"Исключение при прошивке: {str(e)}")
                messagebox.showerror("Ошибка", f"Произошла ошибка: {str(e)}")
            finally:
                if os.path.exists(temp_firmware_path):
                    try:
                        os.remove(temp_firmware_path)
                        logging.debug(f"Временный файл удален: {temp_firmware_path}")
                    except Exception as e:
                        logging.error(f"Ошибка удаления временного файла: {str(e)}")

        threading.Thread(target=flash, daemon=True).start()

    def get_firmware_path(self, firmware_name='esp32_current.bin'):
        try:
            if getattr(sys, 'frozen', False):
                base_path = sys._MEIPASS
            else:
                base_path = os.path.dirname(os.path.abspath(__file__))

            firmware_path = os.path.join(base_path, firmware_name)
            logging.debug(f"Проверяется путь к прошивке: {firmware_path}")

            if not os.path.exists(firmware_path):
                logging.error(f"Файл прошивки не найден: {firmware_path}")
                return None
            if not os.access(firmware_path, os.R_OK):
                logging.error(f"Файл прошивки недоступен для чтения: {firmware_path}")
                return None

            logging.debug(f"Файл прошивки найден: {firmware_path}")
            return firmware_path
        except Exception as e:
            logging.error(f"Ошибка в get_firmware_path: {str(e)}")
            return None

    def select_firmware_file(self):
        file_path = filedialog.askopenfilename(
            title="Выберите файл прошивки",
            filetypes=(("Bin файлы", "*.bin"), ("Все файлы", "*.*"))
        )
        if file_path:
            logging.debug(f"Выбран файл прошивки: {file_path}")
            if not os.path.exists(file_path):
                logging.error(f"Выбранный файл не существует: {file_path}")
                messagebox.showerror("Ошибка", f"Выбранный файл не найден: {file_path}")
                return
            if not os.access(file_path, os.R_OK):
                logging.error(f"Выбранный файл недоступен для чтения: {file_path}")
                messagebox.showerror("Ошибка", f"Выбранный файл недоступен: {file_path}")
                return
            self.firmware_path.set(file_path)

    def create_context_menu(self):
        self.context_menu = tk.Menu(self.window, tearoff=0)
        self.context_menu.add_command(label="Копировать", command=self.copy_text)
        self.response_text.bind("<Button-3>", self.show_context_menu)

    def show_context_menu(self, event):
        self.context_menu.post(event.x_root, event.y_root)

    def copy_text(self, event=None):
        try:
            self.response_text.clipboard_clear()
            selected_text = self.response_text.selection_get()
            self.response_text.clipboard_append(selected_text)
        except tk.TclError:
            pass

    def load_image(self, path, size):
        try:
            original_image = Image.open(path)
            resized_image = original_image.resize(size)
            return ImageTk.PhotoImage(resized_image)
        except Exception as e:
            logging.error(f"Ошибка загрузки изображения {path}: {str(e)}")
            return None

    def refresh_ports(self):
        try:
            self.ports = serial.tools.list_ports.comports()
            port_names = [port.device for port in self.ports]
            self.port_combobox['values'] = port_names
            logging.debug(f"Обновлен список портов: {port_names}")
        except Exception as e:
            logging.error(f"Ошибка при обновлении портов: {str(e)}")

    def connect_device(self):
        selected_port = self.port_combobox.get()
        if selected_port:
            try:
                self.serial_connection = serial.Serial(selected_port, 9600, timeout=0.1)
                self.response_text.insert(tk.END, f"Подключено к {selected_port}\n")
                self.window.after(100, self.read_serial_data)
                logging.debug(f"Подключено к порту: {selected_port}")
            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка подключения: {str(e)}\n")
                logging.error(f"Ошибка подключения к порту {selected_port}: {str(e)}")

    def set_device_id(self):
        device_id = self.device_id_entry.get().strip()
        if device_id.isdigit():
            command = f"setdeviceid {device_id}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                logging.debug(f"Отправлена команда setdeviceid: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setdeviceid без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат Device ID. Введите число.")
            logging.error("Неправильный формат Device ID")

    def set_client_id(self):
        client_id = self.client_id_entry.get().strip()
        if client_id:
            command = f"setclientid {client_id}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                logging.debug(f"Отправлена команда setclientid: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setclientid без подключения")
        else:
            messagebox.showerror("Ошибка", "Client ID не может быть пустым.")
            logging.error("Client ID пустой")

    def set_ssid(self):
        ssid = self.ssid_entry.get().strip()
        if ssid:
            command = f"setssid {ssid}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                logging.debug(f"Отправлена команда setssid: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setssid без подключения")
        else:
            messagebox.showerror("Ошибка", "SSID не может быть пустым.")
            logging.error("SSID пустой")

    def set_password(self):
        password = self.password_entry.get().strip()
        if password:
            command = f"setpassword {password}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                logging.debug(f"Отправлена команда setpassword: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setpassword без подключения")
        else:
            messagebox.showerror("Ошибка", "Password не может быть пустым.")
            logging.error("Password пустой")

    def set_url(self):
        url = self.url_entry.get().strip()
        if url and url.startswith("http://"):
            command = f"seturl {url}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                logging.debug(f"Отправлена команда seturl: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки seturl без подключения")
        else:
            messagebox.showerror("Ошибка", "Введите корректный URL (например, http://80.80.101.123:503/data).")
            logging.error("Некорректный формат URL")

    def get_settings(self):
        if self.serial_connection and self.serial_connection.is_open:
            command = "getsettings\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, "Запрос настроек датчика тока отправлен.\n")
            logging.debug("Отправлен запрос getsettings")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки getsettings без подключения")

    def read_serial_data(self):
        if self.serial_connection and self.serial_connection.is_open and self.reading_enabled:
            try:
                if self.serial_connection.in_waiting > 0:
                    response = self.serial_connection.read(self.serial_connection.in_waiting).decode('utf-8',
                                                                                                     errors='ignore').strip()
                    if response:
                        self.response_text.insert(tk.END, f"Получено: {response}\n")
                        self.response_text.see(tk.END)
                        logging.debug(f"Получено из serial: {response}")
                self.window.after(50, self.read_serial_data)
            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка чтения: {str(e)}\n")
                logging.error(f"Ошибка чтения из serial: {str(e)}")
                if self.serial_connection.is_open:
                    self.serial_connection.close()
                self.serial_connection = None

    def toggle_diagnostics(self):
        if self.serial_connection and self.serial_connection.is_open:
            if self.diagnostics_var.get():
                self.reading_enabled = True
                self.response_text.insert(tk.END, "Диагностика включена. Монитор порта активен.\n")
                logging.debug("Диагностика включена")
                self.window.after(100, self.read_serial_data)  # Возобновляем чтение
            else:
                self.reading_enabled = False
                self.response_text.insert(tk.END, "Диагностика выключена. Монитор порта остановлен.\n")
                logging.debug("Диагностика выключена")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка переключения диагностики без подключения")

    def show_tooltip(self, event, text):
        try:
            self.tooltip_window = tk.Toplevel(self.window)
            self.tooltip_window.wm_overrideredirect(True)
            x, y = event.x_root + 10, event.y_root + 10
            self.tooltip_window.wm_geometry(f"+{x}+{y}")
            tooltip_label = tk.Label(self.tooltip_window, text=text, background="yellow", relief="solid", borderwidth=1,
                                     padx=5, pady=3, font=("Arial", 10, "bold"))
            tooltip_label.pack()
        except Exception as e:
            logging.error(f"Ошибка отображения tooltip: {str(e)}")

    def hide_tooltip(self, event):
        if self.tooltip_window:
            self.tooltip_window.destroy()
            self.tooltip_window = None

if __name__ == "__main__":
    root = tk.Tk()
    app = CurrentSensorConfigurator(root)
    root.mainloop()