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


class BLEBASAConfigurator:
    def __init__(self, root):
        self.window = Toplevel(root)
        self.window.title("Конфигурация BLE_BASA")
        self.window.iconphoto(False, PhotoImage(file=resources.get_icon_path()))

        self.window.resizable(True, True)
        self.window.wm_minsize(650, 650)

        self.window.grid_rowconfigure(0, weight=1)
        self.window.grid_columnconfigure(0, weight=1, minsize=300)
        self.window.grid_columnconfigure(1, weight=1)
        self.window.grid_columnconfigure(2, weight=1)

        image_frame = ttk.Frame(self.window, padding=0, borderwidth=2, relief="solid")
        image_frame.grid(column=0, row=0, columnspan=1, sticky='nsew', padx=5, pady=5)

        image_path = resources.get_image_path('baza.png')
        self.image = self.load_image(image_path, (100, 100))
        self.image_label = tk.Label(image_frame, image=self.image)
        self.image_label.pack(pady=5)

        self.tooltip_label = ttk.Label(image_frame, text="?", font=("Arial", 12, "bold"), foreground="blue",
                                       cursor="hand2")
        self.tooltip_label.place(relx=1.0, rely=1.0, anchor='se', x=-5, y=-5)
        self.tooltip_label.bind("<Enter>", lambda e: self.show_tooltip(e,
                                                                       "BLE basa может работать в двух режимах:\n1.Метки- в этом режиме база находит метку с лучшим сигналом согласно настраиваемых параметров \n поиск осуществляется среди внесенных МАС-адресов \n позиция найденого устройства в массиве передается в качестве номера метки \n перед началом конфигурации желательно удалить все МАС-адреса \n 2.ДУТ - в этом режиме база находит различные датчики и транслирует их данные по rs-485 в соответствующие адреса\n Например, если вам нужно передавать данные от BLE ДУТ по rs-485 по третьему адресу \n тогда необходимо добавить МАС-адрес BLE ДУТа в поле Введите МАС-адрес №3"))
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

        self.firmware_path = tk.StringVar(value=self.get_firmware_path('esp32.bin'))
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

        self.tab_control = ttk.Notebook(self.window)
        self.tab_control.grid(column=0, row=3, columnspan=5, sticky='nsew', padx=5, pady=5)

        self.labels_tab = ttk.Frame(self.tab_control)
        self.tab_control.add(self.labels_tab, text='Метки')

        mac_frame = ttk.Frame(self.labels_tab, borderwidth=2, relief="solid", padding=0)
        mac_frame.grid(column=0, row=0, columnspan=5, sticky='ew', padx=5, pady=5)
        mac_frame.grid_columnconfigure(1, weight=1)

        self.mac_label = ttk.Label(mac_frame, text="Введите MAC-адрес")
        self.mac_label.grid(column=0, row=0, padx=10, pady=10)

        self.mac_entry = ttk.Entry(mac_frame, width=20)
        self.mac_entry.grid(column=1, row=0, padx=10, pady=10, sticky="ew")
        self.mac_entry.bind('<KeyRelease>', self.format_mac_address)

        self.send_button = ttk.Button(mac_frame, text="Добавить", command=self.send_mac_address)
        self.send_button.grid(column=2, row=0, padx=10, pady=10)

        mac_control_frame = ttk.Frame(self.labels_tab, borderwidth=2, relief="solid", padding=0)
        mac_control_frame.grid(column=0, row=1, columnspan=5, sticky='ew', padx=5, pady=5)
        mac_control_frame.grid_columnconfigure(0, weight=1)
        mac_control_frame.grid_columnconfigure(1, weight=1)

        self.get_macs_button = ttk.Button(mac_control_frame, text="Получить список MAC-адресов",
                                          command=self.get_mac_addresses)
        self.get_macs_button.grid(column=0, row=0, padx=10, pady=10, sticky='ew')

        self.delete_all_button = ttk.Button(mac_control_frame, text="Удалить все MAC-адреса",
                                            command=self.delete_all_mac_addresses)
        self.delete_all_button.grid(column=1, row=0, padx=10, pady=10, sticky='ew')

        delete_by_position_frame = ttk.Frame(self.labels_tab, borderwidth=2, relief="solid", padding=0)
        delete_by_position_frame.grid(column=0, row=2, columnspan=5, sticky='ew', padx=5, pady=5)
        delete_by_position_frame.grid_columnconfigure(1, weight=1)

        self.position_label = ttk.Label(delete_by_position_frame, text="Введите позицию для удаления:")
        self.position_label.pack(side=tk.LEFT, padx=5, pady=5)

        self.position_entry = ttk.Entry(delete_by_position_frame, width=5)
        self.position_entry.pack(side=tk.LEFT, padx=5, pady=5, expand=True)

        self.delete_position_button = ttk.Button(delete_by_position_frame, text="Удалить MAC по позиции",
                                                 command=self.delete_mac_by_position)
        self.delete_position_button.pack(side=tk.LEFT, padx=5, pady=5)

        settings_frame = ttk.Frame(self.labels_tab, borderwidth=2, relief="solid", padding=0)
        settings_frame.grid(column=0, row=3, columnspan=5, sticky='ew', padx=5, pady=5)
        settings_frame.grid_columnconfigure(1, weight=1)

        self.update_interval_label = ttk.Label(settings_frame, text="Период обновления (с):")
        self.update_interval_label.grid(column=0, row=0, padx=10, pady=10)

        self.update_interval_entry = ttk.Entry(settings_frame, width=10)
        self.update_interval_entry.grid(column=1, row=0, padx=10, pady=10)

        self.update_interval_button = ttk.Button(settings_frame, text="Установить", command=self.update_interval)
        self.update_interval_button.grid(column=2, row=0, padx=10, pady=10)

        self.tooltip_label = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"), foreground="blue",
                                       cursor="hand2")
        self.tooltip_label.grid(column=3, row=0, padx=(0, 5), sticky='w')
        self.tooltip_label.bind("<Enter>", lambda e: self.show_tooltip(e,
                                                                       "Период обновления должен быть больше времени удержания.\n По умолчанию 60 секунд"))
        self.tooltip_label.bind("<Leave>", self.hide_tooltip)

        self.best_rssi_time_label = ttk.Label(settings_frame, text="Время удержания метки (с):")
        self.best_rssi_time_label.grid(column=0, row=1, padx=10, pady=10)

        self.best_rssi_time_entry = ttk.Entry(settings_frame, width=10)
        self.best_rssi_time_entry.grid(column=1, row=1, padx=10, pady=10)

        self.best_rssi_time_button = ttk.Button(settings_frame, text="Установить", command=self.update_best_rssi_time)
        self.best_rssi_time_button.grid(column=2, row=1, padx=10, pady=10)

        self.best_rssi_time_tooltip = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"), foreground="blue",
                                                cursor="hand2")
        self.best_rssi_time_tooltip.grid(column=3, row=1, padx=(0, 5), sticky='w')
        self.best_rssi_time_tooltip.bind("<Enter>", lambda e: self.show_tooltip(e,
                                                                                "Укажите время в секундах, в течение которого удерживается метка.По умолчани 30"))
        self.best_rssi_time_tooltip.bind("<Leave>", self.hide_tooltip)

        self.best_rssi_count_label = ttk.Label(settings_frame, text="Количество повторений:")
        self.best_rssi_count_label.grid(column=0, row=2, padx=10, pady=10)

        self.best_rssi_count_entry = ttk.Entry(settings_frame, width=10)
        self.best_rssi_count_entry.grid(column=1, row=2, padx=10, pady=10)

        self.best_rssi_count_button = ttk.Button(settings_frame, text="Установить", command=self.update_best_rssi_count)
        self.best_rssi_count_button.grid(column=2, row=2, padx=10, pady=10)

        self.best_rssi_count_tooltip = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"),
                                                 foreground="blue", cursor="hand2")
        self.best_rssi_count_tooltip.grid(column=3, row=2, padx=(0, 5), sticky='w')
        self.best_rssi_count_tooltip.bind("<Enter>", lambda e: self.show_tooltip(e,
                                                                                 "Укажите сколько раз метка должна появится в эфире и быть лучшей за время удержания.По умолчани 3"))
        self.best_rssi_count_tooltip.bind("<Leave>", self.hide_tooltip)

        self.best_rssi_threshold_label = ttk.Label(settings_frame, text="Уровень сигнала (дБ):")
        self.best_rssi_threshold_label.grid(column=0, row=3, padx=10, pady=10)

        self.best_rssi_threshold_entry = ttk.Entry(settings_frame, width=10)
        self.best_rssi_threshold_entry.grid(column=1, row=3, padx=10, pady=10)

        self.best_rssi_threshold_button = ttk.Button(settings_frame, text="Установить",
                                                     command=self.update_best_rssi_threshold)
        self.best_rssi_threshold_button.grid(column=2, row=3, padx=10, pady=10)

        self.best_rssi_threshold_tooltip = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"),
                                                     foreground="blue", cursor="hand2")
        self.best_rssi_threshold_tooltip.grid(column=3, row=3, padx=(0, 5), sticky='w')
        self.best_rssi_threshold_tooltip.bind(
            "<Enter>",
            lambda e: self.show_tooltip(
                e,
                "Укажите уровень сигнала в дБм (от -125 до +5).\n"
                "По умолчанию: -90 дБм\n\n"
                "Примеры значений RSSI:\n"
                "  -30 дБм  — рядом (около 10 см)\n"
                "  -50 дБм  — 1–2 метра\n"
                "  -70 дБм  — 4–6 метров\n"
                "  -80 дБм  — 7–10 метров\n"
                "  -90 дБм  — 10+ метров\n"
            )
        )
        self.best_rssi_threshold_tooltip.bind("<Leave>", self.hide_tooltip)

        self.network_address_label = ttk.Label(settings_frame, text="Сетевой адрес:")
        self.network_address_label.grid(column=0, row=4, padx=10, pady=10)

        self.network_address_entry = ttk.Entry(settings_frame, width=10)
        self.network_address_entry.grid(column=1, row=4, padx=10, pady=10)

        self.network_address_button = ttk.Button(settings_frame, text="Установить", command=self.update_network_address)
        self.network_address_button.grid(column=2, row=4, padx=10, pady=10)

        self.network_address_tooltip = ttk.Label(settings_frame, text="?", font=("Arial", 12, "bold"),
                                                 foreground="blue", cursor="hand2")
        self.network_address_tooltip.grid(column=3, row=4, padx=(0, 5), sticky='w')
        self.network_address_tooltip.bind("<Enter>", lambda e: self.show_tooltip(e,
                                                                                 "Укажите сетевой адрес устройства.По умолчани 4"))
        self.network_address_tooltip.bind("<Leave>", self.hide_tooltip)

        self.get_settings_button = ttk.Button(settings_frame, text="Считать настройки", command=self.get_settings)
        self.get_settings_button.grid(column=0, row=5, columnspan=3, padx=10, pady=10, sticky='ew')

        self.settings_tab = ttk.Frame(self.tab_control)
        self.tab_control.add(self.settings_tab, text='ДУТ')

        mac_control_frame = ttk.Frame(self.settings_tab, borderwidth=2, relief="solid", padding=10)
        mac_control_frame.grid(column=0, row=0, columnspan=5, sticky='ew', padx=5, pady=5)
        mac_control_frame.grid_columnconfigure(0, weight=1)
        mac_control_frame.grid_columnconfigure(1, weight=1)

        self.get_macs_button = ttk.Button(mac_control_frame, text="Получить список MAC-адресов",
                                          command=self.get_mac_addresses)
        self.get_macs_button.grid(column=0, row=0, padx=0, pady=0, sticky='ew')

        self.delete_all_button = ttk.Button(mac_control_frame, text="Удалить все MAC-адреса",
                                            command=self.delete_all_mac_addresses)
        self.delete_all_button.grid(column=1, row=0, padx=0, pady=0, sticky='ew')

        dut_frame = ttk.Frame(self.settings_tab, borderwidth=2, relief="solid", padding=10)
        dut_frame.grid(column=0, row=1, columnspan=5, sticky='ew', padx=5, pady=5)

        self.mac_entries = []
        self.byte_entries = []
        self.device_comboboxes = []
        self.custom_byte_entries = []

        for i in range(6):
            mac_label = ttk.Label(dut_frame, text=f"Введите MAC-адрес №{i + 1}")
            mac_label.grid(column=0, row=i, padx=0, pady=0)

            mac_entry = ttk.Entry(dut_frame, width=15)
            mac_entry.grid(column=1, row=i, padx=10, pady=10, sticky="ew")
            mac_entry.bind('<KeyRelease>', self.format_mac_address)
            self.mac_entries.append(mac_entry)

            device_combobox = ttk.Combobox(dut_frame, values=["Escort", "другое"], state="readonly", width=10)
            device_combobox.grid(column=2, row=i, padx=10, pady=10, sticky="ew")
            self.device_comboboxes.append(device_combobox)

            custom_byte_entry = ttk.Entry(dut_frame, width=5)
            custom_byte_entry.grid(column=3, row=i, padx=10, pady=10, sticky="ew")
            custom_byte_entry.grid_remove()
            self.custom_byte_entries.append(custom_byte_entry)

            device_combobox.bind("<<ComboboxSelected>>", lambda event, index=i: self.on_device_selection(index))

            send_button = ttk.Button(dut_frame, text="Добавить",
                                     command=lambda index=i: self.send_mac_address_dut(index))
            send_button.grid(column=4, row=i, padx=10, pady=10)

        self.tab_control.bind("<<NotebookTabChanged>>", self.on_tab_change)

        diagnostics_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        diagnostics_frame.grid(column=2, row=0, rowspan=8, sticky='nsew', padx=5, pady=5)

        self.diagnostics_var = tk.BooleanVar()
        self.diagnostics_checkbutton = ttk.Checkbutton(diagnostics_frame, text="Диагностика RS-485",
                                                       variable=self.diagnostics_var, command=self.toggle_diagnostics)
        self.diagnostics_checkbutton.pack(padx=10, pady=10)

        self.enable_diagnostics = tk.BooleanVar(value=False)  # Диагностика по умолчанию ВЫКЛЮЧЕНА!
        self.diagnostic_checkbox = ttk.Checkbutton(diagnostics_frame, text="Диагностика",
                                                   variable=self.enable_diagnostics)
        self.diagnostic_checkbox.pack(pady=5, padx=5, anchor="center")

        response_frame = ttk.Frame(self.window, borderwidth=2, relief="solid", padding=10)
        response_frame.grid(column=1, row=0, rowspan=8, sticky='nsew', padx=5, pady=5)

        self.response_text = tk.Text(response_frame, height=30, width=25)
        self.response_text.pack(fill=tk.BOTH, expand=True)
        self.response_text.bind("<Control-c>", self.copy_text)
        self.create_context_menu()

        # Добавлена кнопка очистки
        clear_button = ttk.Button(response_frame, text="Очистить", command=self.clear_response)
        clear_button.pack(pady=5)

        self.ports = []
        self.serial_connection = None
        self.tooltip_window = None

        self.refresh_ports()

    def get_firmware_path(self, firmware_name='esp32.bin'):
        """Получение пути к файлу прошивки."""
        try:
            if getattr(sys, 'frozen', False):
                base_path = sys._MEIPASS
                logging.debug(f"PyInstaller: Временная папка: {base_path}")
                logging.debug(f"Файлы в sys._MEIPASS: {os.listdir(base_path)}")
            else:
                base_path = os.path.dirname(os.path.abspath(__file__))
                logging.debug(f"Исходный код: Папка проекта: {base_path}")

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
        """Открыть диалог выбора файла прошивки."""
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

    def flash_esp32(self):
        """Прошивка ESP32 с использованием выбранного COM-порта и файла."""
        port = self.port_combobox.get()
        firmware_file = self.firmware_path.get() or self.get_firmware_path('esp32.bin')

        if not port:
            messagebox.showerror("Ошибка", "Выберите COM-порт для прошивки.")
            logging.error("COM-порт не выбран")
            return
        if not firmware_file or not os.path.exists(firmware_file):
            messagebox.showerror("Ошибка",
                                 f"Файл прошивки не найден: {firmware_file}. Убедитесь, что файл добавлен в сборку или выберите другой файл.")
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
            real_time_out = RealTimeOutput(self.window, self.progress_bar, self.percent_label, original_stdout)

            sys.stdout = real_time_out
            sys.stderr = real_time_out

            try:
                if getattr(sys, 'frozen', False):
                    esptool_path = os.path.join(sys._MEIPASS, 'run_esptool.py')
                else:
                    esptool_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'run_esptool.py')

                logging.debug(f"Путь к run_esptool.py: {esptool_path}")
                if not os.path.exists(esptool_path):
                    logging.error(f"Файл run_esptool.py не найден: {esptool_path}")
                    sys.stdout = original_stdout
                    sys.stderr = original_stderr
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
                success = False

                try:
                    run_esptool.main()
                    success = True
                except SystemExit:
                    pass
                except Exception as e:
                    logging.error(f"Исключение в run_esptool: {str(e)}")
                    real_time_out.write(f"Исключение в esptool: {str(e)}")

                output_str = real_time_out.getvalue()

                # Восстанавливаем вывод
                sys.stdout = original_stdout
                sys.stderr = original_stderr

                if success and "Hash of data verified" in output_str:
                    self.window.after(0, lambda: self.progress_bar.configure(value=100))
                    self.window.after(0, lambda: self.percent_label.configure(text="100%"))
                    messagebox.showinfo("Успех", "Прошивка завершена успешно!")
                    logging.debug("Прошивка успешно завершена")
                else:
                    logging.error(f"Ошибка прошивки:\n{output_str}")
                    messagebox.showerror("Ошибка",
                                         f"Прошивка завершилась с ошибкой:\n{output_str or 'Нет вывода от esptool'}")
            except Exception as e:
                sys.stdout = original_stdout
                sys.stderr = original_stderr
                logging.error(f"Исключение при прошивке: {str(e)}")
                messagebox.showerror("Ошибка", f"Произошла ошибка: {str(e)}")
            finally:
                sys.stdout = original_stdout
                sys.stderr = original_stderr
                if os.path.exists(temp_firmware_path):
                    try:
                        os.remove(temp_firmware_path)
                        logging.debug(f"Временный файл удален: {temp_firmware_path}")
                    except Exception as e:
                        logging.error(f"Ошибка удаления временного файла: {str(e)}")

        threading.Thread(target=flash, daemon=True).start()

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
                # Важно: использовать правильные настройки порта
                self.serial_connection = serial.Serial(
                    port=selected_port,
                    baudrate=9600,
                    timeout=2,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    bytesize=serial.EIGHTBITS
                )

                self.serial_connection.reset_input_buffer()
                self.serial_connection.reset_output_buffer()

                self.response_text.insert(tk.END, f"Подключено к {selected_port}\n")
                self.response_text.see(tk.END)

                self.window.after(100, self.read_serial_data)
                logging.debug(f"Подключено к порту: {selected_port}")

                self.window.after(500, lambda: self.get_settings())

            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка подключения: {str(e)}\n")
                self.response_text.see(tk.END)
                logging.error(f"Ошибка подключения к порту {selected_port}: {str(e)}")

    def on_device_selection(self, index):
        selected_device = self.device_comboboxes[index].get()
        if selected_device == "другое":
            self.custom_byte_entries[index].grid()
            self.window.geometry("1250x650")
        else:
            self.custom_byte_entries[index].grid_remove()
            self.window.geometry("1050x650")

    def send_mac_address_dut(self, index):
        self.send_set_mode(1)
        mac_address = self.mac_entries[index].get()
        position = index + 1
        device_type = self.device_comboboxes[index].get()

        if device_type == "Escort":
            byte_number = 8
        else:
            custom_byte_value = self.custom_byte_entries[index].get()
            if not custom_byte_value.strip():
                logging.error("Поле для байта пустое")
                self.response_text.insert(tk.END, "Ошибка: поле для байта пустое.\n")
                return
            try:
                byte_number = int(custom_byte_value)
            except ValueError:
                logging.error("Некорректное значение для байта")
                self.response_text.insert(tk.END, "Ошибка: введено некорректное значение для байта.\n")
                return

        if self.validate_mac(mac_address):
            command = f"newdut {mac_address} {position} {byte_number}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлено: {command.strip()}\n")
                logging.debug(f"Отправлена команда newdut: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки newdut без подключения")
        else:
            logging.error("Неправильный формат MAC-адреса")
            self.response_text.insert(tk.END, "Ошибка: неправильный формат MAC-адреса.\n")

    def send_mac_address(self):
        if self.serial_connection and self.serial_connection.is_open:
            mac_address = self.mac_entry.get().strip()
            if self.validate_mac(mac_address):
                command = f"newmac {mac_address}\n"
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлено: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда newmac: {command.strip()}")
            else:
                messagebox.showerror("Ошибка", "Неправильный формат MAC-адреса.")
                logging.error("Неправильный формат MAC-адреса")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки newmac без подключения")

    def get_mac_addresses(self):
        if self.serial_connection and self.serial_connection.is_open:
            current_tab = self.tab_control.select()
            mode = 0 if current_tab == str(self.labels_tab) else 1
            command = "getmacs\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, "Запрос списка MAC-адресов отправлен.\n")
            self.send_set_mode(mode)
            logging.debug(f"Отправлен запрос getmacs, mode: {mode}")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки getmacs без подключения")

    def delete_all_mac_addresses(self):
        if self.serial_connection and self.serial_connection.is_open:
            command = "deleteall\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, "Отправлена команда на удаление всех MAC-адресов\n")
            self.send_set_mode(0)
            logging.debug("Отправлена команда deleteall")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки deleteall без подключения")

    def delete_mac_by_position(self):
        position = self.position_entry.get().strip()
        if position.isdigit():
            position = int(position)
            command = f"deleteat {position}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.send_set_mode(0)
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда на удаление MAC по позиции: {position}\n")
                logging.debug(f"Отправлена команда deleteat: {position}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки deleteat без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат позиции. Введите число.")
            logging.error("Неправильный формат позиции")

    def update_interval(self):
        interval = self.update_interval_entry.get().strip()
        if interval.isdigit():
            command = f"setupdate {interval}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда setupdate: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setupdate без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат. Введите число.")
            logging.error("Неправильный формат для setupdate")

    def update_best_rssi_time(self):
        time = self.best_rssi_time_entry.get().strip()
        if time.isdigit():
            command = f"setbtime {time}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда setbtime: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setbtime без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат. Введите число.")
            logging.error("Неправильный формат для setbtime")

    def update_best_rssi_count(self):
        count = self.best_rssi_count_entry.get().strip()
        if count.isdigit():
            command = f"setbcount {count}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда setbcount: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setbcount без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат. Введите число.")
            logging.error("Неправильный формат для setbcount")

    def update_network_address(self):
        network_address = self.network_address_entry.get().strip()
        if network_address.isdigit():
            command = f"setnetaddr {network_address}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда setnetaddr: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setnetaddr без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат сетевого адреса. Введите число.")
            logging.error("Неправильный формат для setnetaddr")

    def update_best_rssi_threshold(self):
        threshold = self.best_rssi_threshold_entry.get().strip()
        if threshold.lstrip('-').isdigit():
            command = f"setbthreshold {threshold}\n"
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлена команда: {command.strip()}\n")
                self.send_set_mode(0)
                logging.debug(f"Отправлена команда setbthreshold: {command.strip()}")
            else:
                self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
                logging.warning("Попытка отправки setbthreshold без подключения")
        else:
            messagebox.showerror("Ошибка", "Неправильный формат. Введите число.")
            logging.error("Неправильный формат для setbthreshold")

    def format_mac_address(self, event):
        mac = event.widget.get().replace(':', '')
        new_mac = ':'.join([mac[i:i + 2] for i in range(0, len(mac), 2)])
        event.widget.delete(0, tk.END)
        event.widget.insert(0, new_mac[:17])

    def validate_mac(self, mac):
        parts = mac.split(':')
        if len(parts) != 6:
            return False
        for part in parts:
            if len(part) != 2 or not part.isalnum() or not all(c in '0123456789ABCDEFabcdef' for c in part):
                return False
        return True

    def send_set_mode(self, mode_value):
        if self.serial_connection and self.serial_connection.is_open:
            command = f"setmode {mode_value}\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, f"Отправлено: {command.strip()}\n")
            logging.debug(f"Отправлена команда setmode: {command.strip()}")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки setmode без подключения")

    def get_settings(self):
        if self.serial_connection and self.serial_connection.is_open:
            command = "getsettings\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, "Запрос текущих настроек отправлен.\n")
            self.send_set_mode(0)
            logging.debug("Отправлен запрос getsettings")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки getsettings без подключения")

    def read_serial_data(self):
        if self.serial_connection and self.serial_connection.is_open:
            try:
                while self.serial_connection.in_waiting > 0:
                    # Читаем байты
                    data = self.serial_connection.read(self.serial_connection.in_waiting)

                    # Проверяем, может ли это быть текстом
                    try:
                        # Пробуем декодировать как UTF-8 с разными стратегиями
                        response = data.decode('utf-8', errors='ignore')

                        # Удаляем непечатаемые символы, кроме пробелов и переносов
                        cleaned_response = []
                        for char in response:
                            if char.isprintable() or char in '\n\r\t':
                                cleaned_response.append(char)

                        response = ''.join(cleaned_response).strip()

                        if response:
                            # Проверяем, не является ли это бинарными данными
                            # Если строка содержит много непечатаемых символов или похожа на hex дамп, пропускаем
                            if self.is_likely_text(response):
                                if self.enable_diagnostics.get():
                                    self.response_text.insert(tk.END, f"Получено: {response}\n")
                                    self.response_text.see(tk.END)
                                self.process_response(response)
                            elif self.enable_diagnostics.get():
                                # Показываем только краткую информацию о бинарных данных
                                if len(data) > 10:
                                    hex_preview = ' '.join(f'{b:02X}' for b in data[:10])
                                    self.response_text.insert(tk.END,
                                                              f"[Бинарные данные: {len(data)} байт, {hex_preview}...]\n")
                                else:
                                    hex_str = ' '.join(f'{b:02X}' for b in data)
                                    self.response_text.insert(tk.END, f"[Бинарные данные: {hex_str}]\n")

                    except UnicodeDecodeError:
                        # Это точно бинарные данные
                        if self.enable_diagnostics.get():
                            if len(data) > 10:
                                hex_preview = ' '.join(f'{b:02X}' for b in data[:10])
                                self.response_text.insert(tk.END,
                                                          f"[Бинарные данные: {len(data)} байт, {hex_preview}...]\n")
                            else:
                                hex_str = ' '.join(f'{b:02X}' for b in data)
                                self.response_text.insert(tk.END, f"[Бинарные данные: {hex_str}]\n")

            except Exception as e:
                logging.error(f"Ошибка чтения serial данных: {str(e)}")
                if self.enable_diagnostics.get():
                    self.response_text.insert(tk.END, f"Ошибка чтения: {str(e)}\n")

            # Продолжаем чтение
            self.window.after(100, self.read_serial_data)

    def is_likely_text(self, text):
        """Определяет, похожа ли строка на текстовые данные, а не на бинарные."""
        if not text:
            return False

        # Если строка слишком короткая, проверяем содержание
        if len(text) < 5:
            return True

        # Проверяем процент печатаемых символов
        printable_count = sum(1 for c in text if c.isprintable() or c in '\n\r\t')
        printable_ratio = printable_count / len(text)

        if printable_ratio < 0.7:  # Меньше 70% печатаемых символов
            return False

        # Проверяем на hex дамп (много шестнадцатеричных символов и пробелов)
        hex_chars = set('0123456789ABCDEFabcdef ')
        hex_count = sum(1 for c in text if c in hex_chars)
        hex_ratio = hex_count / len(text)

        if hex_ratio > 0.8 and len(text) > 20:  # Похоже на hex дамп
            return False

        # Проверяем на RS485 ответы
        if text.startswith(('3E ', '31 ', '>', '0x3E', '0x31')):
            return False

        # Проверяем, содержит ли строка кириллицу или латиницу
        has_letters = any(c.isalpha() for c in text)
        has_cyrillic = any('\u0400' <= c <= '\u04FF' for c in text)  # Кириллица

        return has_letters or has_cyrillic

    def process_response(self, response):
        lines = response.split('\n')
        for line in lines:
            line = line.strip()
            if line:
                if line.startswith("Сетевой адрес:") or line.startswith("Режим работы:") or line.startswith("Период"):
                    self.response_text.insert(tk.END, f"{line}\n")
                    logging.debug(f"Получена настройка: {line}")
                elif not line.startswith("[Бинарные данные"):
                    self.response_text.insert(tk.END, f"{line}\n")
                    logging.debug(f"Получена строка: {line}")

    def on_tab_change(self, event):
        active_tab_index = self.tab_control.index(self.tab_control.select())
        if active_tab_index == 1:
            self.window.geometry("1050x650")
        else:
            self.window.geometry("850x650")

    def send_setmode_command(self, mode):
        if self.serial_connection and self.serial_connection.is_open:
            command = f"setmode {mode}\n"
            self.serial_connection.write(command.encode('utf-8'))
            self.response_text.insert(tk.END, f"Отправлено: {command.strip()}\n")
            logging.debug(f"Отправлена команда setmode: {command.strip()}")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка отправки setmode без подключения")

    def toggle_diagnostics(self):
        if self.serial_connection and self.serial_connection.is_open:
            if self.diagnostics_var.get():
                command = "debugMode 1\n"
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, "Диагностика RS-485 включена.\n")
                logging.debug("Диагностика RS-485 включена")
            else:
                command = "debugMode 0\n"
                self.serial_connection.write(command.encode('utf-8'))
                self.response_text.insert(tk.END, "Диагностика RS-485 выключена.\n")
                logging.debug("Диагностика RS-485 выключена")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")
            logging.warning("Попытка переключения диагностики без подключения")

    def show_tooltip(self, event, text):
        try:
            if self.tooltip_window:
                self.tooltip_window.destroy()

            self.tooltip_window = tk.Toplevel(self.window)
            self.tooltip_window.wm_overrideredirect(True)
            x, y = event.x_root + 10, event.y_root + 10
            self.tooltip_window.wm_geometry(f"+{x}+{y}")
            tooltip_label = tk.Label(self.tooltip_window, text=text, background="yellow",
                                     relief="solid", borderwidth=1, padx=5, pady=3,
                                     font=("Arial", 10, "bold"), justify=tk.LEFT)
            tooltip_label.pack()
        except Exception as e:
            logging.error(f"Ошибка отображения tooltip: {str(e)}")

    def hide_tooltip(self, event):
        if self.tooltip_window:
            self.tooltip_window.destroy()
            self.tooltip_window = None

    def clear_response(self):
        """Очистить поле с ответами"""
        self.response_text.delete(1.0, tk.END)


if __name__ == "__main__":
    root = tk.Tk()
    app = BLEBASAConfigurator(root)
    root.mainloop()