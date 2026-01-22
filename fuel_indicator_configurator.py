import tkinter as tk
from tkinter import ttk, PhotoImage, Toplevel
import serial
import serial.tools.list_ports
import resources
from PIL import Image, ImageTk  # Импортируем PIL для работы с изображениями


class FuelIndicatorConfigurator:
    def __init__(self, root):
        self.window = Toplevel(root)
        self.window.title("Конфигурация индикатора топлива")
        self.window.iconphoto(False, PhotoImage(file=resources.get_icon_path()))
        self.window.geometry("500x650")  # Увеличиваем ширину для размещения изображений

        # Создаем canvas и scrollbar
        self.canvas = tk.Canvas(self.window)
        self.scrollbar = ttk.Scrollbar(self.window, orient="vertical", command=self.canvas.yview)
        self.scrollable_frame = ttk.Frame(self.canvas)

        # Настраиваем canvas
        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )

        self.canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")

        # Связываем scrollbar с canvas
        self.canvas.configure(yscrollcommand=self.scrollbar.set)

        # Располагаем canvas и scrollbar
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.scrollbar.grid(row=0, column=1, sticky="ns")

        # Устанавливаем конфигурацию для растягивания
        self.window.grid_rowconfigure(0, weight=1)
        self.window.grid_columnconfigure(0, weight=1)

        # Фрейм для изображения, размещаем его в верхней части
        image_frame = ttk.Frame(self.scrollable_frame, padding=10)
        image_frame.grid(column=0, row=0, columnspan=5, sticky='nsew', padx=5, pady=5)

        # Загрузка и изменение размера изображения tablo2.png
        image_path = resources.get_image_path('tablo2.png')
        self.image = self.load_image(image_path, (150, 150))  # Измените размер под ваши нужды

        # Метка с изображением
        self.image_label = tk.Label(image_frame, image=self.image)
        self.image_label.pack(pady=5)  # Центрируем изображение в верхней части

        # Фрейм для настройки COM-порта с красной рамкой
        port_frame = ttk.Frame(self.scrollable_frame, borderwidth=2, relief="solid", padding=10)
        port_frame.grid(column=0, row=1, columnspan=5, sticky='ew', padx=5, pady=5)

        self.port_label = ttk.Label(port_frame, text="Выберите COM-порт:")
        self.port_label.grid(column=0, row=0, padx=10, pady=10)

        self.port_combobox = ttk.Combobox(port_frame, state="readonly", width=10)
        self.port_combobox.grid(column=1, row=0, columnspan=2, padx=10, pady=10)

        self.refresh_button = ttk.Button(port_frame, text="Обновить", command=self.refresh_ports)
        self.refresh_button.grid(column=3, row=0, padx=5, pady=10)

        self.connect_button = ttk.Button(port_frame, text="Подключить", command=self.connect_device)
        self.connect_button.grid(column=4, row=0, padx=5, pady=10)

        # Фрейм для ввода импульсов с красной рамкой
        impulse_frame = ttk.Frame(self.scrollable_frame, borderwidth=2, relief="solid", padding=10)
        impulse_frame.grid(column=0, row=2, columnspan=5, sticky='ew', padx=5, pady=5)

        self.impulse_label = ttk.Label(impulse_frame, text="Введите количество\nимпульсов на литр:")
        self.impulse_label.grid(column=0, row=0, padx=10, pady=10, sticky='w')  # Используем sticky='w' для выравнивания по левому краю

        # Создание метки с символом вопроса
        self.tooltip_label = ttk.Label(impulse_frame, text="?", font=("Arial", 12), cursor="hand2")
        self.tooltip_label.grid(row=0, column=2)

        # Привязка событий для отображения и скрытия подсказки
        self.tooltip_label.bind("<Enter>", self.show_tooltip)
        self.tooltip_label.bind("<Leave>", self.hide_tooltip)

        self.tooltip_window = None

        # Создаем комбобокс для импульсов
        self.impulse_combobox = ttk.Combobox(impulse_frame, state="readonly")
        self.impulse_combobox['values'] = ["Счетчик OGM-50P", "Другое"]
        self.impulse_combobox.grid(column=1, row=0, padx=10, pady=10)

        # Поле для ввода собственного значения, по умолчанию скрыто
        self.custom_impulse_entry = ttk.Entry(impulse_frame, width=13)
        self.custom_impulse_entry.grid(column=2, row=0, padx=10, pady=10)
        self.custom_impulse_entry.grid_remove()  # Скрываем его по умолчанию

        self.impulse_combobox.bind("<<ComboboxSelected>>", self.on_impulse_selection)

        self.litres_label = ttk.Label(impulse_frame, text="Введите общий литраж:")
        self.litres_label.grid(column=0, row=1, padx=10, pady=10)

        self.litres_entry = ttk.Entry(impulse_frame, width=13)
        self.litres_entry.grid(column=1, row=1, columnspan=2, padx=10, pady=10)

        # Кнопка для отправки данных
        self.send_button = ttk.Button(self.scrollable_frame, text="Отправить", command=self.send_commands)
        self.send_button.grid(column=0, row=3, columnspan=5, padx=10, pady=10)

        # Фрейм для текстового ответа с красной рамкой
        response_frame = ttk.Frame(self.scrollable_frame, borderwidth=2, relief="solid", padding=10)
        response_frame.grid(column=0, row=4, columnspan=5, sticky='ew', padx=5, pady=5)

        self.response_text = tk.Text(response_frame, height=10, width=50)
        self.response_text.pack(fill=tk.BOTH, expand=True)

        self.ports = []
        self.serial_connection = None

        self.refresh_ports()

    def load_image(self, path, size):
        original_image = Image.open(path)
        resized_image = original_image.resize(size)  # Изменяем размер изображения
        return ImageTk.PhotoImage(resized_image)

    def refresh_ports(self):
        self.ports = serial.tools.list_ports.comports()
        port_names = [port.device for port in self.ports]
        self.port_combobox['values'] = port_names

    def connect_device(self):
        selected_port = self.port_combobox.get()
        if selected_port:
            try:
                self.serial_connection = serial.Serial(selected_port, 9600, timeout=1)
                self.response_text.insert(tk.END, f"Подключено к {selected_port}\n")
                self.window.after(100, self.read_serial_data)
            except Exception as e:
                self.response_text.insert(tk.END, f"Ошибка подключения: {str(e)}\n")

    def read_serial_data(self):
        if self.serial_connection and self.serial_connection.is_open:
            while self.serial_connection.in_waiting > 0:
                response = self.serial_connection.readline().decode('utf-8').strip()
                self.response_text.insert(tk.END, f"Получено: {response}\n")
            self.window.after(100, self.read_serial_data)

    def send_commands(self):
        if self.serial_connection and self.serial_connection.is_open:
            impulse_value = self.custom_impulse_entry.get().strip() if self.impulse_combobox.get() == "Другое" else "7.184"  # Пример: замените на нужное значение
            litres_value = self.litres_entry.get().strip()

            if litres_value:
                reset_command = f"reset {litres_value}\n"
                self.serial_connection.write(reset_command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлено: {reset_command}\n")

            if impulse_value:
                set_command = f"set {impulse_value}\n"
                self.serial_connection.write(set_command.encode('utf-8'))
                self.response_text.insert(tk.END, f"Отправлено: {set_command}\n")
        else:
            self.response_text.insert(tk.END, "Не подключено ни одно устройство\n")

    def on_impulse_selection(self, event):
        selected_value = self.impulse_combobox.get()
        if selected_value == "Другое":
            self.custom_impulse_entry.grid()  # Показываем поле для ввода
            self.custom_impulse_entry.focus()  # Устанавливаем фокус на поле ввода
        else:
            self.custom_impulse_entry.grid_remove()  # Скрываем поле для ввода
            self.custom_impulse_entry.delete(0, tk.END)  # Очищаем поле ввода

    def show_tooltip(self, event):
        # Отобразить текст подсказки
        self.tooltip_window = tk.Toplevel(self.window)
        self.tooltip_window.wm_overrideredirect(True)
        self.tooltip_window.wm_geometry(f"+{event.x_root + 10}+{event.y_root + 10}")
        tooltip_label = tk.Label(self.tooltip_window,
                                 text="Введите количество импульсов на литр для точности измерений.")
        tooltip_label.pack()

    def hide_tooltip(self, event):
        # Скрыть текст подсказки
        if self.tooltip_window:
            self.tooltip_window.destroy()
            self.tooltip_window = None


# Далее ваш основной код для запуска приложения
if __name__ == "__main__":
    root = tk.Tk()
    app = FuelIndicatorConfigurator(root)
    root.mainloop()
