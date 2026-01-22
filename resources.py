import os

def get_icon_path():
    return os.path.join(os.path.dirname(__file__), 'tablo2.png')

def get_image_path(filename):
    return os.path.join(os.path.dirname(__file__), filename)