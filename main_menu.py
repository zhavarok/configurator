import tkinter as tk
from tkinter import PhotoImage, ttk
from fuel_indicator_configurator import FuelIndicatorConfigurator
from ble_basa_configurator import BLEBASAConfigurator
from attendance_system import AttendanceSystemConfigurator
from current_sensor_configurator import CurrentSensorConfigurator
from temperature_sensor_configurator import TemperatureSensorConfigurator  # Добавляем новый конфигуратор
import resources
import sys
from PIL import Image, ImageTk

class MainMenu:
    def __init__(self, root):
        self.root = root
        self.root.title("Главное меню")

        self.root.geometry("850x250")  # Увеличиваем ширину для размещения пяти устройств
        self.root.resizable(False, False)

        try:
            self.root.iconphoto(False, PhotoImage(file=resources.get_icon_path()))
        except Exception as e:
            print(f"Ошибка загрузки иконки: {e}")

        self.main_frame = ttk.Frame(self.root, borderwidth=2, relief="solid", padding=10)
        self.main_frame.pack(expand=True, fill=tk.BOTH, padx=10, pady=10)

        self.create_image_label_frame(self.main_frame, 'tablo2.png', "Индикатор топлива", 0)
        self.create_image_label_frame(self.main_frame, 'baza.png', "BLE_BASA", 1)
        self.create_image_label_frame(self.main_frame, 'clock.png', "Система учета времени", 2)
        self.create_image_label_frame(self.main_frame, 'current.png', "Датчик тока", 3)
        self.create_image_label_frame(self.main_frame, 'temp.png', "Датчик температуры", 4)  # Новое устройство

    def create_image_label_frame(self, parent, image_name, label_text, column):
        frame = ttk.Frame(parent, borderwidth=2, relief="solid", padding=10)
        frame.grid(column=column, row=0, padx=10, pady=10, sticky='n')

        image_path = resources.get_image_path(image_name)
        image = self.load_image(image_path, (100, 100))

        image_label = tk.Label(frame, image=image)
        image_label.image = image
        image_label.pack(pady=0, anchor='center')

        label = tk.Label(frame, text=label_text)
        label.pack(pady=0, anchor='center')

        image_label.bind("<Button-1>", lambda event, text=label_text: self.open_config(event, text))

    def load_image(self, path, size):
        try:
            original_image = Image.open(path)
            resized_image = original_image.resize(size)
            return ImageTk.PhotoImage(resized_image)
        except Exception as e:
            print(f"Ошибка загрузки изображения {path}: {e}")
            return None

    def open_config(self, event, text):
        if text == "Индикатор топлива":
            FuelIndicatorConfigurator(self.root)
        elif text == "BLE_BASA":
            BLEBASAConfigurator(self.root)
        elif text == "Система учета времени":
            AttendanceSystemConfigurator(self.root)
        elif text == "Датчик тока":
            CurrentSensorConfigurator(self.root)
        elif text == "Датчик температуры":
            TemperatureSensorConfigurator(self.root)  # Открываем конфигуратор датчика температуры

if __name__ == "__main__":
    root = tk.Tk()
    app = MainMenu(root)
    root.mainloop()