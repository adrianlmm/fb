# Pendientes de la gema fb (no publicar 0.10.5 tal cual)

## 1. Segfault: sentencia cacheada reutilizada + desalojo + DDL

**Estado**: RESUELTO en 0.10.7 con el anclaje de cursores cacheados
(`cached_cursors`, marcado en `fb_connection_mark`): la cache esta reactivada y
los repros + la suite pasan. 0.10.6 (cache desactivada) queda como version
conservadora por si acaso.

**Sintoma**: segfault (SIGSEGV) al ejecutar un DDL (`DROP TABLE`, `ALTER TABLE`,
`CREATE TABLE`) despues de que la cache de sentencias se haya llenado y desalojado.

**Disparador minimo reproducible** (script `fases.rb`, version 0.10.5):
    2000 inserts con el MISMO SQL  ->  500 inserts con SQL DISTINTO (500 > 64, hay
    desalojo)  ->  DROP TABLE   =>  segfault en el DROP.

Sin desalojo (un solo SQL repetido) o sin DDL no revienta. En 0.10.4 tampoco
revienta: lo introdujo la cache de sentencias de 0.10.5.

**Diagnostico** (gdb, ruby 4.0.0):
    #6  fb_stmt_cache_destroy (...) at fb.c:710     <- xfree(e), se libera el NODO
    #7  fb_stmt_cache_invalidate (...) at fb.c:758
    #8  cursor_execute2 (...) at fb.c:3095          <- el DDL
El recorrido de la lista es correcto (en la traza instrumentada, `en_lista=1` en
los 455 destroy), asi que la corrupcion del heap la provoca una liberacion/drop
previa desde otro camino.

**Ya arreglado y verificado** (en el arbol de trabajo, sin publicar): el cursor con
`cache` hacia `isc_dsql_free_statement(..., DSQL_drop)` de una sentencia que
pertenece a la entrada en `fb_cursor_free` (camino del GC). Ahora solo hace
`DSQL_close`. Con eso pasan los casos de desalojo + DDL y la suite del proyecto
sigue en 129 runs / 1034 assertions / 0 failures (1 error preexistente:
`test_role_support`, que necesita el usuario Firebird `rubytest`).

**Sigue fallando**: el disparador completo (2000 mismo SQL + 500 distintos + DROP).
Siguiente paso sugerido: bisecar por fases con `MALLOC_CHECK_=3` y timeouts cortos
(el manejador de crash de Ruby no termina, hay que usar SIGKILL).

**Recomendacion**: no publicar 0.10.5 hasta cerrarlo; 0.10.4 es la ultima que no
revienta.


## Actualizacion 0.10.6

- `FB_STMT_CACHE_MAX` = 0: la cache de sentencias queda desactivada (con el
  porque en el propio fb.c). Se pierde ~2x en SQL repetido; el batch por
  protocolo no depende de ella.
- Comprobado con la cache puesta -> segfault, y quitada -> pasa:
    2000 mismo SQL + 500 distintos + DROP           => fin OK
    secuencia original completa (2000+500+20000+SELECT+COUNT+DROP) => fin OK
    suite del proyecto                              => 129 runs, 0 failures
- Valgrind (con la cache puesta) localiza el fallo en
  `fb_stmt_cache_destroy` (fb.c:704, el DSQL_drop) escribiendo sobre un bloque
  que el GC ya habia liberado (reservado como FbCursor por `connection_cursor`).
  No es un doble drop del mismo handle (2445 drops registrados, ninguno
  repetido) ni un nodo duplicado (todos los destroy con en_lista=1): es un
  problema de vida util entre cursor y cache.
- Tres arreglos que se quedan en 0.10.6 (todos verificados):
  1. el cursor con cache ya no hace DSQL_drop de la sentencia de la entrada;
  2. ni DSQL_close: en el GC no toca handles ajenos, solo se desvincula;
  3. cursor_close/fb_cursor_drop* dejan el handle a cero tras dropearlo.
- Para volver a activar la cache: que el cursor no ceda nunca su sentencia
  (la cache solo para el camino de una sola llamada) o que la conexion marque
  (rb_gc_mark) los cursores con sentencia cacheada.


## 2. Consultas de mas de 50 columnas con la cache activa

**Estado**: RESUELTO en 0.10.8 (afecta a 0.10.5 y 0.10.7; 0.10.6 se libra
porque tiene la cache desactivada).

**Sintoma**: con un SELECT de mas de SQLDA_COLSINIT (50) columnas, la primera
ejecucion va bien y la SEGUNDA ejecucion del MISMO SQL (es decir, con la
sentencia ya en la cache) devuelve un resultado incorrecto (`query` devuelve 0
en lugar de las filas) o mata el proceso: `free(): invalid size`, `[BUG] Aborted`
o SIGSEGV. Nondeterminista: depende de si el heap reutilizo o no la memoria
liberada, asi que tambien puede "pasar" por casualidad.

**Causa**: en `cursor_execute2`, el SQLDA de salida se le entregaba a la entrada
de cache (`fb_stmt_cache_add` guarda el puntero) ANTES de reasignarlo cuando hay
mas de 50 columnas. La entrada quedaba apuntando al SQLDA de 50 ya liberado: la
segunda ejecucion leia `sqld` de memoria liberada (fb.c:3196) y, si salia 0, la
sentencia se trataba como no-consulta; o bien comparaba basura en `sqln < cols`
y hacia `xfree` de un puntero ya liberado (fb.c:3221, doble free), y
`fb_cursor_fetch_prep` recorre `sqld` entradas sobre una estructura de 50
(desbordamiento).

**Repro**: `ruby /tmp/fb_50cols_repro.rb 60` (tabla de 60 columnas y el mismo
`SELECT *` tres veces). Valgrind sobre la version sin arreglar:
`Invalid read of size 2 at cursor_execute2 (fb.c:3196)` y
`Invalid free() ... at cursor_execute2 (fb.c:3221)` sobre un bloque de 8.024
bytes (`XSQLDA_LENGTH(50)`) liberado en fb.c:3221.

**Arreglo**: reasignar el SQLDA de salida antes de registrar la entrada en la
cache (dentro del bloque de prepare), y por si acaso, en el punto donde antes se
hacia la reasignacion, si el SQLDA venia de la cache dejar la entrada apuntando
al nuevo. Entradas de mas de 50 parametros de entrada no se ven afectadas: su
SQLDA se reasigna antes de registrar.

**Verificacion** (0.10.8): 50/51/60/90/200/1000 columnas con el mismo SQL
repetido dan siempre los valores correctos; valgrind no reporta lecturas ni
liberaciones invalidas en fb.c; la suite del proyecto sigue sin fallos
(`test_more_than_50_columns_repeated_query`, 1000 columnas x 10 vueltas, es el
caso nuevo: con la version rota falla en la segunda vuelta en 6 de 6 corridas, y
con el arreglo pasa en 6 de 6). La suite no tenia ningun
caso con mas de 50 columnas, por eso paso desapercibido al activar la cache en
0.10.5.


## 3. "invalid statement handle" en un DDL tras consultas cacheadas

**Estado**: RESUELTO en 0.10.9 (afecta a 0.10.5 y 0.10.7 en adelante; 0.10.6 se
libra porque tiene la cache desactivada). Es el resto del disparador que en el
punto 1 figuraba como "sigue fallando".

**Sintoma**: despues de ejecutar N veces el MISMO SQL (sentencia en la cache), un
DDL (`DROP TABLE`, `CREATE TABLE`...) falla con `Fb::Error ... invalid statement
handle` al ejecutarse. Depende del estado del heap: con una tabla ancha (1000
columnas) se ve en la suite completa; en otros casos no se ve, pero valgrind
delata el destrozo: `Invalid write of size 4` dentro de
`isc_dsql_free_statement`, viniendo de `fb_stmt_cache_destroy` →
`fb_stmt_cache_invalidate` → `cursor_execute2`.

**Causa** (aislada con un programa C minimo, `/tmp/fbprobe3.c`):
`isc_dsql_alloc_statement2()`/`isc_dsql_prepare()` guardan la DIRECCION de la
variable `stmt` que se les pasa, y `isc_dsql_free_statement(..., DSQL_drop)`
escribe NULL tanto en la variable que se le pasa como en aquella que tiene
apuntada:

    alloc+prepare: v1=1
    tras drop via copia: copy=0 v1=0    <- zeroea tambien la variable original

La cache guarda una COPIA del handle (`e->stmt = fb_cursor->stmt`) mientras el
dueno de la direccion original es el cursor que preparo la sentencia. Ese cursor
se libera y el GC recicla su memoria... y Ruby suele dar la MISMA direccion al
siguiente cursor. Cuando la invalidacion (un DDL) hace DSQL_drop de la entrada,
el NULL de Firebird aterriza en el campo `stmt` del cursor nuevo -> el DDL
siguiente se ejecuta con handle 0 y falla.

**Arreglo**: la entrada de cache recuerda su cursor dueno (`FbStmtCache.owner`) y
el dueno NO se despina al cerrar el cursor: sigue anclado
(`fb_connection->cached_cursors`, lo mismo que evita el segfault de 0.10.7)
hasta que la entrada se destruye, momento en que se hace el DSQL_drop y despues
se despina. Asi la direccion que Firebird escribe siempre es memoria viva.

**Verificacion** (0.10.9): la suite completa con el caso nuevo (que ahora incluye
el `DROP TABLE` final) pasa 4 de 4 corridas: 131 tests, 1072 assertions,
0 failures, 1 error preexistente (`test_role_support`, falta el usuario Firebird
`rubytest`); sin el arreglo, ese mismo caso fallaba con "invalid statement
handle". Valgrind sobre la suite: 0 `Invalid write` de `isc_dsql_free_statement`.

