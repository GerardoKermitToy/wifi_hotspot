# Поддержка WPA3

## Как включить

1. Обновите драйвер адаптера из доверенного источника.
2. В WiFi Studio выберите адаптер, источник Интернета и поддерживаемый диапазон.
3. Выберите **WPA3** и включите точку доступа. Для совместимости с WPA2-клиентами можно использовать **WPA2 / WPA3**.

WPA3 работает не только на 6 ГГц: при поддержке драйвером доступен и на 2,4 ГГц. Поддержка WPA3 при подключении к роутеру не гарантирует её в режиме точки доступа.

## Проверка возможностей

```powershell
netsh wlan show wirelesscapabilities
```

Важен флаг `P2P Sae on GO`: он относится к SAE в роли Wi-Fi Direct Group Owner. Обычный флаг `SAE Authentication` сам по себе недостаточен.

Приложение проверяет поддержку через `IsAuthenticationKindSupportedAsync` и не понижает защиту автоматически. Даже при положительном ответе окончательный результат запуска возвращает Windows.

## Фактическая проверка 2026-11-22

На тестовом USB-адаптере исходный драйвер поддерживал WPA3 только в клиентском режиме. После обновления драйвера пользователем Windows разрешила чистый WPA3 и переходный WPA2/WPA3 для хот-спота.

Для обоих режимов на **2,4 ГГц** успешно выполнены:

- CLI: `Start → On → 5 секунд → Stop → Off`;
- GUI: выбор защиты, запуск, состояние `On`, отказ второго процесса вмешиваться, остановка до `Off`;
- сравнение сохранённой конфигурации Windows до/после CLI-тестов: без изменений.

Правки реестра, обход проверок и изменение настроек драйвера не понадобились. Результат относится к проверенному устройству и драйверу, а не ко всем адаптерам.

Подключение отдельного WPA3-клиента, SAE-обмен, DHCP и передача Интернета **не проверялись**. Успешный `StartTetheringAsync` и состояние `On` не заменяют эти проверки.

## Источники

- [Аутентификация хот-спота WPA2 / WPA3](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.tetheringwifiauthenticationkind)
- [Проверка поддерживаемой защиты](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.networkoperatortetheringsessionaccesspointconfiguration.isauthenticationkindsupportedasync)
- [Сессионная конфигурация Mobile Hotspot](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.networkoperatortetheringsessionaccesspointconfiguration)
