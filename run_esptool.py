import esptool
import sys

def main():
    sys.argv[0] = "esptool"  # Заменяем имя скрипта для esptool
    esptool.main()

if __name__ == "__main__":
    main()