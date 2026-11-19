# Proyecto de Sistemas Operativos
## Proyecto 4: Copy-on-Write y MiniDB (xv6)

Implementación validada el 16 de noviembre de 2026 sobre el commit local de reconstrucción `8f8550a`. Las cuatro suites pasan. 

### Integrantes

- Cristian Cabezas
- Johan Peña

### Curso

Sistemas Operativos

### Semestre

2026-2

---

# Descripción

Se implementa Copy-on-Write sobre xv6-riscv 06aad25 con base_proyecto3.patch. fork comparte páginas físicas; las páginas de usuario escribibles quedan sin W y con COW. La primera escritura resuelve la propiedad: copia si existen otros dueños o restaura escritura si queda uno. copyout usa la misma resolución. cowstat publica memoria global y contadores por proceso. MiniDB crea un hijo que guarda el estado de la tabla mientras el padre aplica transferencias sobre su propia vista.

---

# Archivos modificados

| Archivo | Modificación |
|---|---|
| Makefile | Incluye pgtbltest, cowtest y minidb; enlaza forktest con user.ld y su biblioteca reducida para evitar un segmento RWX |
| kernel/kernel.ld | Separa segmentos ELF RX y RW sin silenciar advertencias |
| kernel/cowstat.h | Estructura pública de estadísticas |
| kernel/kalloc.c | Referencias por página, locks y estadísticas globales |
| kernel/defs.h | Prototipos de referencias, estadísticas y resolución CoW |
| kernel/riscv.h | Bit 8 PTE_COW |
| kernel/vm.c | Fork con páginas compartidas, resolución y copyout |
| kernel/trap.c | Atención de fallos de escritura CoW |
| kernel/proc.h | Contadores por proceso |
| kernel/proc.c | Inicialización de contadores al asignar un proceso |
| kernel/syscall.h y kernel/syscall.c | Registro de syscall 25, cowstat |
| kernel/sysproc.c | cowstat; validación de rango de pgaccess sin desbordamiento y sfence_vma al limpiar A |
| user/user.h y user/usys.pl | Interfaz y stub de cowstat |
| user/minidb.c | Escritura y verificación del snapshot y transferencias concurrentes |

Las versiones completas de los archivos modificados incluyen los cambios de la base del docente. memlayout.h y ulib.c no se entregan porque no se modificaron respecto a esa base. Los programas de prueba y minidb.h se obtienen del docente, sin cambios.

---

# Integración con xv6

1. Clonar xv6-riscv y ejecutar `git switch -c proyecto4 06aad25`.
2. Aplicar `git apply base_proyecto3.patch` del docente.
3. Copiar los archivos de esta entrega en sus rutas.
4. Copiar los originales del docente: user/cowtest.c, user/minidb.h, user/pgtbltest.c y user/usertests.c. No sustituir minidb.c por el esqueleto después de copiar nuestra versión.
5. Compilar con `make qemu CPUS=3`.
6. Ejecutar cowtest, minidb consulta 3, minidb snapshot 2000, minidb snapshot 0, minidb mapa, pgtbltest y usertests -q.
7. Copiar ejecutar_pruebas.py del docente a la raíz de xv6 y ejecutar `python3 ejecutar_pruebas.py` para obtener las cuatro evidencias originales.

---

# Respuestas a las preguntas

## 1. PTE antes y después de una escritura CoW

Captura de `pruebas/minidb.txt`, commit `8f8550a`, dirección virtual `0x5000`:

| Momento | PTE | PA | V | R | W | X | U | A | D | COW |
|---|---|---|---|---|---|---|---|---|---|---|
| Antes | 0x21fb61d3 | 0x87ed8000 | 1 | 1 | 0 | 0 | 1 | 1 | 1 | 1 |
| Después | 0x21fca0d7 | 0x87f28000 | 1 | 1 | 1 | 0 | 1 | 1 | 1 | 0 |

El bit D ya estaba marcado antes de esta escritura: es estado conservado de la página, no una indicación de que W siga habilitado. Las direcciones físicas pueden variar entre ejecuciones.

Los bits se decodifican con `(pte >> bit) & 1`: V=0, R=1, W=2, X=3, U=4, A=6, D=7, COW=8. La dirección física se obtiene con `(pte >> 10) << 12`. En una página de datos compartida esperamos V=R=U=COW=1, W=0 y X=0. Después de la escritura del hijo esperamos W=1 y COW=0. A y D deben leerse de la captura real, no suponerse a partir del permiso W.

El hijo obtiene otra página física porque el padre conserva su referencia a la original. La PTE del padre sigue apuntando a la original, sin W y con COW. Cuando el padre escriba, si ya es el único dueño, recuperará W sin copiar. Si aún hay otros dueños, necesitará su propia copia.

## 2. Páginas sin PTE_W y USYSCALL en fork()

Las páginas originalmente de solo lectura se comparten sin añadir COW. Una página que ya tenía COW lo conserva en forks sucesivos. La guarda requiere atención adicional: en esta base puede tener W, pero no U; por eso solo marcamos COW cuando existen tanto W como U. El resolvedor vuelve a exigir U. Una escritura del usuario en código, guarda o USYSCALL sigue siendo inválida.

USYSCALL queda fuera del rango ordinario copiado por uvmcopy. allocproc reserva una página propia y guarda el PID nuevo, y proc_pagetable la mapea de solo lectura. Si el hijo compartiera la del padre, ugetpid devolvería el PID del padre; actualizar esa página compartida con el PID del hijo dañaría la lectura del padre. El trapframe también permanece privado.

## 3. Carrera en el conteo de referencias

Supongamos que una página tiene dos dueños y count=2. CPU0 empieza a liberarla y lee 2. CPU1 empieza a liberar la otra referencia y también lee 2. Ambas calculan 1 y lo escriben. Nadie observa cero: no queda ningún dueño, pero el conteo dice uno y la página nunca vuelve a la lista libre.

refs.lock protege cada lectura/modificación del arreglo. La interacción con la lista libre usa kmem.lock; cuando se toman ambos, el orden es kmem y luego refs. cowfault suelta el lock de conteos antes de reservar, copiar o liberar. Esto evita recursión del lock. Las páginas todavía referenciadas no se rellenan con basura ni se ponen en la lista libre.

## 4. Consistencia del snapshot

fork captura la vista de la tabla para el hijo antes de que el padre aplique transferencias. Al escribir el padre en una página compartida, CoW conserva la página original para el hijo. El hijo escribe directamente esa vista a snap.db, por lo que su checksum corresponde al estado anterior al fork. La comprobación relee el archivo, verifica encabezado, longitud exacta y checksum y comprueba conservación del saldo actual.

El generador concentra la actividad en las primeras 1024 cuentas (8 páginas), con transferencias ocasionales fuera de ellas. No se escriben las 64 páginas de la tabla; además, una página ya privada no se copia de nuevo por cada transferencia. Si el hijo ya terminó cuando el padre escribe, puede resolverse con una sola referencia. Los contadores medidos son del proceso entero, de modo que alguna página auxiliar también puede intervenir.

Si el padre esperara al hijo antes de transferir, sus páginas quedarían como únicas referencias y las escrituras de la tabla restaurarían W sin copias físicas. Pero eso detendría la operación durante la escritura y no cumpliría la concurrencia pedida.

En la ejecución registrada, snapshot 2000 aplicó 1978 transferencias y rechazó 22: 30 páginas tocadas, 30 fallos y 30 copias. El archivo tuvo 262160 bytes y checksum 0x6915D295; la tabla actual 0x4AB3E209, conservando el saldo. Snapshot 0 registró cero páginas tocadas, fallos y copias, manteniendo el checksum inicial.

## 5. Aceleración y planificador

Tres repeticiones por combinación; mediana [mínimo, máximo] en ticks:

| K | CPUS=1 | CPUS=3 |
|---|---|---|
| 1 | 23 [23, 25] | 24 [24, 28] |
| 2 | 24 [23, 25] | 13 [13, 14] |
| 3 | 23 [23, 24] | 11 [9, 11] |
| 4 | 24 [23, 26] | 10 [9, 11] |
| 8 | 24 [24, 24] | 12 [11, 13] |

Con CPUS=3, la aceleración mediana de K=1 a K=3 fue 2.18x. Con K=8 la mediana fue 12 ticks. Los experimentos completos y la comparación de memoria están en REPORTE.pdf; las salidas y CSV se conservan en pruebas/experimentos/.

La consulta crea todos los trabajadores antes de leer los pipes. Con tres CPUs, hasta tres procesos pueden ejecutar la firma al mismo tiempo. Pasar de uno a tres trabajadores puede reducir el tiempo; ocho no crean ocho CPUs y añaden costes de fork, comunicación y planificación. Con una CPU los trabajadores se alternan, sin ejecutar simultáneamente sus cálculos. Round Robin distribuye turnos entre procesos ejecutables; un proceso bloqueado esperando un pipe no está ejecutando la firma. La aceleración exacta depende de la máquina, de QEMU y de la granularidad de uptime.

---

# Resultados de las pruebas

| Suite | Estado |
|---|---|
| cowtest | PASÓ: 11/11; 7 s incluyendo arranque y compilación cuando aplica |
| minidb | PASÓ: 7 comandos verificados; 11 s incluyendo arranque y compilación cuando aplica |
| pgtbltest | PASÓ: 11/11; 23 s incluyendo arranque y compilación cuando aplica |
| usertests -q | PASÓ: ALL TESTS PASSED; 57 s incluyendo arranque y compilación cuando aplica |

Los cuatro archivos de pruebas/ son registros originales, sin edición, obtenidos en Ubuntu/WSL sobre la reconstrucción limpia `8f8550a`. Este es un commit técnico local de validación, no un aporte atribuido a un integrante del grupo. Los cambios posteriores se limitan a documentación y comentarios; no alteran las instrucciones del código probado. La compilación registrada no contiene advertencias. Además, se verificaron 30 consultas en los experimentos y la memoria libre volvió al valor inicial después de wait en todas ellas.

---

# Decisiones de diseño

- Arreglo fijo de referencias para páginas entre KERNBASE y PHYSTOP; las páginas no disponibles para asignación permanecen sin referencias contabilizadas en ese arreglo.
- La estadística sharedpages cuenta páginas físicas con más de una referencia, no número de mapeos ni únicamente páginas COW.
- Los fallos contabilizados son resoluciones exitosas, tanto por traps como por copyout; las copias solo incrementan cuando se reserva una nueva página.
- Los procesos recién asignados inicializan ambos contadores a cero; no heredan contadores del padre.
- Si la copia falla por falta de memoria, se marca el proceso para terminar; la PTE anterior y su referencia quedan intactas hasta limpieza.
- En un fork fallido se desmapean las páginas ya instaladas del hijo. Las páginas del padre que quedaron COW siguen siendo válidas y se resolverán al escribir.
- sfence_vma invalida traducciones después de cambiar permisos y de limpiar bits de acceso.
- El hijo trunca un snap.db previo; la verificación también exige fin de archivo justo después de la tabla.
- El buffer de lectura del snapshot se reserva con sbrk, evitando colocar 256 KiB en la pila de una página.

---

# Limitaciones conocidas

- Las suites suministradas y los experimentos pasaron; su cobertura no constituye una demostración formal para todas las ejecuciones posibles.
- El escaneo del arreglo de referencias en cowstat es lineal y bloquea brevemente el asignador; prioriza claridad y coherencia de la medición.
- El snapshot comprobado es consistente lógicamente; no se implementa recuperación transaccional ante cortes de energía ni restauración al iniciar.
- La memoria se informa antes de fork, con trabajadores y después de wait. La comparación sin CoW del reporte es una estimación de páginas de tabla copiadas, no una medición de otro kernel.

---

# Declaración de uso de IA

Se utilizó Chatgpt como apoyo para implementar y comprender partes del proyecto, corregir errores y resolver problemas de compilación. Los integrantes revisan el trabajo y asumen la responsabilidad del contenido entregado.
