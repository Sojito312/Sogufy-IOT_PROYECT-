# SOGUFY para ESP32

El ESP32 dibuja la interfaz en la ILI9341 por SPI. El iPhone envía el audio al ESP32 por Bluetooth clásico A2DP y recibe los comandos de los botones por AVRCP. El ESP32 consulta la canción actual y descarga su portada por HTTPS mediante la API de Spotify. Se usa la canción de Bluetooth para comprobar que la portada corresponde al audio. Después del alta inicial puedes alimentar el ESP32 con un cargador USB y reproducir desde el iPhone.


## 1. Compilar y cargar el firmware

Abre PowerShell en la raíz del repositorio. El proyecto usa ESP-IDF 6.1 y el ESP32 clásico. Cambia el puerto si tu placa aparece en otro COM.

```powershell
. "C:\esp\v6.1\esp-idf\export.ps1"
idf.py -B build-fiad -D SDKCONFIG=sdkconfig.fiad -D SDKCONFIG_DEFAULTS=sdkconfig.fiad.defaults -D IDF_TARGET=esp32 build
idf.py -B build-fiad -D SDKCONFIG=sdkconfig.fiad -D SDKCONFIG_DEFAULTS=sdkconfig.fiad.defaults -D IDF_TARGET=esp32 -p COM13 flash
```

ESP-IDF descarga cJSON y Tiny JPEG Decoder en la primera compilación. Si necesitas integrar el firmware en la práctica `blink`, ejecuta `Instalar_en_blink.ps1` desde este repositorio. El instalador respalda el `main` de la práctica antes de reemplazarlo.

## 2. Autorizar Spotify y conectar el Wi-Fi

1. En el [dashboard de Spotify Developers](https://developer.spotify.com/dashboard), crea o abre tu app de Web API. Agrega `http://127.0.0.1:8888/callback` a **Redirect URIs** y copia el **Client ID**. El titular de una app en Development Mode necesita Spotify Premium. Si vas a iniciar sesión con otra cuenta, agrégala en **User Management** de la app.
2. Cuando la TFT diga **Configura USB**, ejecuta esto desde la raíz del proyecto, con el ESP32 conectado por USB y el monitor serie cerrado. El Python activado con ESP-IDF trae PySerial. Si aparece `No module named serial`, ejecuta `python -m pip install pyserial`.

   ```powershell
   python .\configurar_fiad.py --port COM13
   ```

3. Introduce el Client ID, tu Wi-Fi de **2.4 GHz** y la contraseña. Autoriza el acceso en el navegador con **la misma cuenta que usará el iPhone**. Espera el mensaje **Configuración guardada**. La contraseña de Wi-Fi y el refresh token se guardan en la memoria NVS del ESP32. El script usa OAuth PKCE y mantiene las credenciales en memoria durante el alta.
4. En el iPhone abre **Configuración → Bluetooth** y conecta **SOGUFY**. En Spotify pon una canción y selecciona **SOGUFY** como salida de audio si iOS te pide elegir. Tras mostrar la portada y comprobar el AUX, conecta la placa al cargador USB para probar el modo autónomo.

## Conexiones

| Función | ESP32 |
| --- | --- |
| TFT ILI9341, VCC y LED | 3V3, según tu montaje actual |
| TFT GND | GND |
| TFT CS, RESET, DC | D27, D22, D21 |
| TFT MOSI, SCK | D23, D18 |
| AUX de bocina amplificada | D25, con el capacitor de 100 µF, resistencia y tierra compartida de tu montaje |
| Anterior | Pulsador entre D32 y GND |
| Pausa / reproducir | Pulsador entre D33 y GND |
| Siguiente | Pulsador entre D13 y GND |
| Perilla de volumen AUX | Centro a D34. Extremos a 3V3 y GND. |

Los tres iconos de la TFT corresponden a los pulsadores físicos. Cada pulsador conecta su GPIO a GND al presionarlo.

## Prueba y diagnóstico

- **Primera prueba Bluetooth sin alta:** en **Configura USB**, presiona el pulsador del centro (D33). Empareja el iPhone y revisa audio, nombre y botones. Al reiniciar vuelve a abrir el modo de configuración si todavía faltan las credenciales.
- **Portada real:** toca una canción en el iPhone. Espera a que la cabecera indique **Spotify** y revisa que la imagen cambie al pulsar siguiente. La API se consulta cada cuatro segundos en condiciones normales. El primer arranque incluye sincronización de la hora para HTTPS.
- **Si aparece BT con el título pero falta la portada:** comprueba que Spotify en el iPhone usa la cuenta que autorizaste y que la canción del teléfono coincide con la de la API. El audio y los botones funcionan por Bluetooth aunque falle la red. Si Spotify no devuelve una imagen JPEG disponible o no cabe en memoria, se muestra el icono de disco.
- **Para ver los errores:** ejecuta `idf.py -B build-fiad -D SDKCONFIG=sdkconfig.fiad -D SDKCONFIG_DEFAULTS=sdkconfig.fiad.defaults -D IDF_TARGET=esp32 -p COM13 monitor`. Busca mensajes del módulo `Spotify`. Sal con **Ctrl+]** antes de volver a usar el configurador USB.
- **Para cambiar de red o volver a autorizar:** mantén presionado **Anterior (D32)** mientras pulsas **EN**. Suelta D32 cuando aparezca **Configura USB** y ejecuta el configurador otra vez.

**Estado de verificación.** Se comprobó la decodificación de portadas de 64 × 64 en el ESP32. El firmware con el ajuste de memoria y la perilla compiló correctamente. Queda pendiente verificar ambos cambios en la placa durante reproducción prolongada.

## Referencias

- [Spotify Web API · Get Currently Playing Track](https://developer.spotify.com/documentation/web-api/reference/get-the-users-currently-playing-track)
- [Spotify · Authorization Code with PKCE](https://developer.spotify.com/documentation/web-api/tutorials/code-pkce-flow)
- [Espressif · Coexistencia de Wi-Fi y Bluetooth](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/coexist.html)
- [Espressif · Tiny JPEG Decoder](https://components.espressif.com/components/espressif/esp_jpeg)
