# SIRAS · Firebase + ESP32

## 1. Crear el proyecto Firebase
1. [console.firebase.google.com](https://console.firebase.google.com) → **Añadir proyecto**.
2. **Build → Realtime Database → Crear base de datos** (elige región Europa; modo bloqueado).
3. **Build → Authentication → Sign-in method**: activa **Correo/contraseña** y **Anónimo**.
4. **Authentication → Users → Add user**: crea `esp32@tu-proyecto.com` con una contraseña. Copia su **UID**.
5. **Realtime Database → Reglas**: pega `database.rules.json` y sustituye `PEGA_AQUI_EL_UID_DEL_ESP32` por ese UID. Publica.
6. **Config. del proyecto → Tus apps → Web (</>)**: copia el objeto `firebaseConfig`.

## 2. Conectar la web
En `index.html`, sustituye el bloque `firebaseConfig` (marcado con `TU_API_KEY`...) por el tuyo. La web entra de forma anónima, escucha `siras/sensores` y `siras/actuadores`, y escribe las órdenes manuales.

## 3. Cargar el ESP32
1. Instala en Arduino IDE: *Firebase Arduino Client Library for ESP8266 and ESP32* (Mobizt), *DHT sensor library* y *Adafruit Unified Sensor*.
2. Abre `siras_esp32.ino` y rellena WiFi, `API_KEY`, `DATABASE_URL`, `USER_EMAIL`, `USER_PASSWORD`.
3. Calibra `SOIL_RAW_DRY/WET`, `BAT_DIV_RATIO` y `TANK_DIST_EMPTY/FULL_CM`.
4. Sube el firmware y abre el monitor serie a 115200.

## Cableado (por defecto)
| Elemento | Pin ESP32 |
|---|---|
| Sonda de humedad capacitiva | GPIO34 |
| Divisor de tensión batería 12 V (30 kΩ / 7,5 kΩ) | GPIO35 |
| DHT22 | GPIO4 |
| JSN-SR04T TRIG / ECHO (ECHO con divisor a 3,3 V) | GPIO5 / GPIO18 |
| Relés: sector A / sector B / válvula principal | GPIO26 / 27 / 25 |

## Esquema de datos
```
/siras
  /sensores    humedad (%), temperatura (°C), bateria (%), nivelTanque (L), ultimaLectura
  /actuadores  modo ("auto" | "manual"), valvulaA, valvulaB, valvulaPrincipal (bool)
  /config      parámetros editables desde la web (ver tabla)
```

## Variables configurables
| Dónde se edita | Variable | Efecto |
|---|---|---|
| Web → ⚙ → Conexión Firebase (localStorage) | apiKey, authDomain, databaseURL, projectId, storageBucket, messagingSenderId, appId | Enlaza la web con tu proyecto |
| Web → ⚙ → ESP32 (`/siras/config`) | moistOn / moistOff | Humedad para abrir / cerrar el riego (histéresis) |
| | tempMax, tankMinL | Bloqueos por temperatura y por tanque casi vacío |
| | maxRiegoMin, lockoutMin | Riego continuo máximo y pausa posterior |
| | pushSec | Cada cuántos segundos envía datos |
| | soilRawDry/Wet, tankDistEmpty/FullCm, tankCapacityL, batVEmpty/Full | Calibración de sonda, tanque y batería |
| Solo firmware (`.ino`) | WIFI_SSID/PASSWORD, API_KEY, DATABASE_URL, USER_EMAIL/PASSWORD | El ESP32 necesita esto para llegar a Firebase |
| Solo firmware (`.ino`) | Pines, BAT_DIV_RATIO, tipo de relé | Hardware |

El ESP32 lee `/siras/config` cada 10 s, limita cada valor a un rango seguro y corrige combinaciones incoherentes. Si el nodo está vacío, publica sus valores por defecto. La web muestra además si el ESP32 está en línea.
- **Modo auto:** el ESP32 decide (abre con humedad < 30 %, cierra al llegar a 35 %, bloquea con T > 30 °C o tanque < 50 L) y publica el estado de las válvulas.
- **Modo manual:** la web escribe las válvulas y el ESP32 las ejecuta.
- **Seguridad:** arranca con todo cerrado, máximo 20 min seguidos de riego y sigue funcionando en local sin WiFi.

## Seguridad
- Solo el usuario del ESP32 puede escribir en `sensores`.
- Cualquier usuario autenticado (incluido el anónimo de la web) puede escribir en `actuadores`, así que quien conozca la URL de la web podría abrir válvulas. Para producción, conviene añadir login de operador en la web y limitar `actuadores` a su UID.
