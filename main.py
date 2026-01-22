import customtkinter as ctk
from main_menu import MainMenu
import sys
import os

if getattr(sys, 'frozen', False):
    print(f"Временная папка PyInstaller: {sys._MEIPASS}")
    print(f"Файлы в папке: {os.listdir(sys._MEIPASS)}")


if __name__ == "__main__":
    root = ctk.CTk()  # Создаем основное окно с customtkinter
    app = MainMenu(root)
    root.mainloop()
