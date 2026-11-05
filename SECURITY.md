# Seguridad

<!-- >>> app -->
Dragon Radar corre en una placa de desarrollo y sirve una web en tu red local. Cualquiera que
esté en la misma red puede cambiar la escala, el tema, el barrido y el volumen, colocar o quitar
esferas, tocar sonidos, apagar el radar, reiniciar la placa o cambiar el WiFi.

En modo configuración la red `DragonRadar-XXXX` no tiene contraseña, para que el portal funcione
sin fricción en cualquier móvil, y sirve el panel completo además del formulario de WiFi.
Mientras no guardes credenciales, cualquiera cerca puede conectarse y usarla.

Los micrófonos de la placa solo graban cuando lo pide el comando serie `rec` (por USB, para medir
el altavoz); la red no puede activarlos.
<!-- <<< app -->

## Modelo de amenaza

Este firmware no está pensado para exponerse a Internet: la API HTTP no tiene autenticación,
HTTPS ni control de acceso.

- **Red doméstica de confianza.** Quien está en tu WiFi puede hablar con la placa. Si eso te
  preocupa, ponla en una red de invitados o de IoT, y no la publiques con port forwarding ni
  con túneles.
- **Portal cautivo abierto.** El AP de configuración es abierto, pero solo sirve el portal y la
  API local de la propia placa.
- **Credenciales WiFi en NVS.** Se guardan en la flash del ESP32 sin cifrar, como en casi
  cualquier proyecto Arduino. Quien tenga acceso físico al USB puede leerlas. No reutilices una
  contraseña importante.
- **La API no devuelve nunca la contraseña del WiFi**, solo el SSID.
- **Lo que llega por la red se valida** contra rangos reales y se escapa al insertarlo en HTML
  (`textContent` en el cliente, escape en el portal del chip).
- **El instalador web solo escribe binarios publicados por el CI** en GitHub Pages; los `.bin`
  no viven en el repositorio.

## Reportar una vulnerabilidad

Si encuentras algo que vaya más allá de lo anterior (ejecución de código, cuelgue remoto, fuga
de credenciales por la API), repórtalo en privado con *Report a vulnerability* en la pestaña
*Security* del repositorio, no como issue público.

Incluye la versión del firmware (`/api/info` o `status` por serie), la placa y cómo
reproducirlo. Respondo en cuanto puedo; no hay SLA, es un proyecto personal.

## Versiones soportadas

Solo la última release. No hay ramas de mantenimiento.
