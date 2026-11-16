// MiniDB: consultas paralelas y snapshots consistentes de cuentas.
// Sistemas Operativos 2026-2, Universidad EAFIT.
// Consulta y mapa se basan en el esqueleto entregado por el docente.

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/cowstat.h"
#include "user/user.h"
#include "user/minidb.h"

static struct cuenta *tabla;

static void
morir(char *msg)
{
  printf("minidb: error: %s\n", msg);
  exit(1);
}

static void
cargar_tabla(void)
{
  if((tabla = reservar_tabla()) == 0)
    morir("no hay memoria para la tabla");
  for(int i = 0; i < NCUENTAS; i++)
    generar_cuenta(i, &tabla[i]);
}

static uint64
libres(void)
{
  struct cowstat st;
  if(cowstat(&st) < 0)
    morir("cowstat() falló");
  return st.freepages;
}

static void
agregar(struct resultado *r, int desde, int hasta, int con_firma)
{
  memset(r, 0, sizeof(*r));
  r->desde = desde;
  r->hasta = hasta;
  for(int i = desde; i < hasta; i++){
    struct cuenta *c = &tabla[i];
    struct agregado *a = &r->suc[c->sucursal];
    a->cuentas++;
    a->saldo_total += c->saldo;
    if(c->saldo > a->saldo_max)
      a->saldo_max = c->saldo;
    if(con_firma)
      a->firmas += firma(c);
  }
}

static int
leer_todo(int fd, void *buf, int n)
{
  int total = 0;
  while(total < n){
    int r = read(fd, (char *)buf + total, n - total);
    if(r <= 0)
      return -1;
    total += r;
  }
  return total;
}

static int
escribir_todo(int fd, void *buf, int n)
{
  int total = 0;
  while(total < n){
    int r = write(fd, (char *)buf + total, n - total);
    if(r <= 0)
      return -1;
    total += r;
  }
  return total;
}

static void
consulta(int k)
{
  if(k < 1 || k > MAXWORKERS)
    morir("K debe estar entre 1 y 8");
  cargar_tabla();
  printf("consulta: trabajadores=%d\n", k);

  int fds[MAXWORKERS][2];
  uint64 libres0 = libres();
  int t0 = uptime();
  for(int w = 0; w < k; w++){
    if(pipe(fds[w]) < 0)
      morir("pipe() falló");
    int pid = fork();
    if(pid < 0)
      morir("fork() falló");
    if(pid == 0){
      close(fds[w][0]);
      struct resultado r;
      agregar(&r, w * NCUENTAS / k, (w + 1) * NCUENTAS / k, 1);
      r.pid = ugetpid();
      if(escribir_todo(fds[w][1], &r, sizeof(r)) < 0)
        exit(1);
      exit(0);
    }
    close(fds[w][1]);
  }
  uint64 libres1 = libres();

  struct agregado total[NSUCURSALES];
  memset(total, 0, sizeof(total));
  int ok = 1, cubiertas = 0;
  for(int w = 0; w < k; w++){
    struct resultado r;
    if(leer_todo(fds[w][0], &r, sizeof(r)) < 0)
      morir("un trabajador no envió su resultado");
    close(fds[w][0]);
    printf("trabajador pid=%d cuentas=[%d,%d)\n", r.pid, r.desde, r.hasta);
    if(r.desde != w * NCUENTAS / k || r.hasta != (w + 1) * NCUENTAS / k)
      ok = 0;
    cubiertas += r.hasta - r.desde;
    for(int s = 0; s < NSUCURSALES; s++){
      total[s].cuentas += r.suc[s].cuentas;
      total[s].saldo_total += r.suc[s].saldo_total;
      total[s].firmas += r.suc[s].firmas;
      if(r.suc[s].saldo_max > total[s].saldo_max)
        total[s].saldo_max = r.suc[s].saldo_max;
    }
  }
  for(int w = 0; w < k; w++){
    int estado;
    if(wait(&estado) < 0 || estado != 0)
      ok = 0;
  }
  int t1 = uptime();
  uint64 libres2 = libres();

  // Verificación secuencial (sin la firma, que es la parte costosa).
  struct resultado sec;
  agregar(&sec, 0, NCUENTAS, 0);
  uint firmas = 0;
  for(int s = 0; s < NSUCURSALES; s++){
    printf("sucursal %d: cuentas=%d total=%ld max=%d\n", s,
           total[s].cuentas, total[s].saldo_total, total[s].saldo_max);
    if(total[s].cuentas != sec.suc[s].cuentas ||
       total[s].saldo_total != sec.suc[s].saldo_total ||
       total[s].saldo_max != sec.suc[s].saldo_max)
      ok = 0;
    firmas += total[s].firmas;
  }
  if(cubiertas != NCUENTAS)
    ok = 0;
  printf("consulta: firma=0x%x\n", firmas);
  printf("consulta: paginas_libres antes=%ld con_trabajadores=%ld\n", libres0, libres1);
  printf("consulta: ticks=%d\n", t1 - t0);
  printf("consulta: paginas_libres despues_wait=%ld\n", libres2);
  printf("consulta: %s\n", ok ? "OK" : "FALLO");
  exit(ok ? 0 : 1);
}


static uint64
saldo_total(const struct cuenta *t)
{
  uint64 total = 0;
  for(int i = 0; i < NCUENTAS; i++)
    total += t[i].saldo;
  return total;
}

static void
guardar_snapshot(uint checksum)
{
  struct snap_encabezado h = {SNAP_MAGIA, NCUENTAS, checksum, 0};
  int fd = open(SNAP_ARCHIVO, O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0)
    exit(1);
  int ok = escribir_todo(fd, &h, sizeof(h)) == sizeof(h) &&
    escribir_todo(fd, tabla, NCUENTAS * sizeof(*tabla)) ==
      NCUENTAS * sizeof(*tabla);
  if(close(fd) < 0)
    ok = 0;
  exit(ok ? 0 : 1);
}

static void
snapshot(int m)
{
  if(m < 0)
    morir("M debe ser no negativo");
  cargar_tabla();
  uint checksum0 = checksum_tabla(tabla, NCUENTAS);
  uint64 total0 = saldo_total(tabla), mask;
  printf("snapshot: checksum_inicial=0x%x\n", checksum0);
  if(pgaccess(tabla, 64, &mask) < 0)
    morir("pgaccess() fallo");
  int pid = fork();
  if(pid < 0)
    morir("fork() fallo");
  if(pid == 0)
    guardar_snapshot(checksum0);
  printf("snapshot: hijo pid=%d escribiendo snap.db\n", pid);

  struct cowstat antes, despues;
  if(cowstat(&antes) < 0)
    morir("cowstat() fallo");
  uint semilla = 4;
  int aplicadas = 0;
  for(int i = 0; i < m; i++){
    struct transferencia t;
    generar_transferencia(&semilla, &t);
    if(t.origen == t.destino || tabla[t.origen].saldo < t.monto)
      continue;
    tabla[t.origen].saldo -= t.monto;
    tabla[t.destino].saldo += t.monto;
    tabla[t.origen].nmov++;
    tabla[t.destino].nmov++;
    aplicadas++;
  }
  if(cowstat(&despues) < 0)
    morir("cowstat() fallo");
  // Mide los accesos antes de recorrer toda la tabla para verificarla.
  if(pgaccess(tabla, 64, &mask) < 0)
    morir("pgaccess() fallo");
  int tocadas = 0;
  for(int i = 0; i < 64; i++)
    if(mask & (1ULL << i))
      tocadas++;
  printf("snapshot: transferencias=%d aplicadas=%d rechazadas=%d\n",
         m, aplicadas, m - aplicadas);
  printf("snapshot: paginas_tocadas=%d fallos_cow=%ld copias_cow=%ld\n",
         tocadas, despues.cowfaults - antes.cowfaults,
         despues.cowcopies - antes.cowcopies);

  int estado = -1;
  int ok = wait(&estado) == pid && estado == 0;
  int bytes = NCUENTAS * sizeof(*tabla);
  struct cuenta *guardadas = (struct cuenta *)sbrk(bytes);
  if(guardadas == (struct cuenta *)-1)
    morir("sin memoria para verificar snapshot");
  struct snap_encabezado h;
  int fd = open(SNAP_ARCHIVO, O_RDONLY);
  if(fd < 0)
    morir("no se pudo abrir snapshot");
  if(leer_todo(fd, &h, sizeof(h)) != sizeof(h) ||
     leer_todo(fd, guardadas, bytes) != bytes){
    close(fd);
    morir("snapshot incompleto");
  }
  char extra;
  int exacto = read(fd, &extra, 1) == 0;
  if(close(fd) < 0)
    ok = 0;
  uint checksum_archivo = checksum_tabla(guardadas, NCUENTAS);
  int coincide = exacto && h.magia == SNAP_MAGIA && h.n == NCUENTAS &&
    h.reservado == 0 && h.checksum == checksum0 &&
    checksum_archivo == checksum0;
  uint checksum_actual = checksum_tabla(tabla, NCUENTAS);
  int conservado = saldo_total(tabla) == total0;
  printf("snapshot: archivo bytes=%d checksum=0x%x coincide=%s\n",
         bytes + (int)sizeof(h), checksum_archivo, coincide ? "si" : "no");
  printf("snapshot: checksum_actual=0x%x total_conservado=%s\n",
         checksum_actual, conservado ? "si" : "no");
  ok = ok && coincide && conservado;
  if(sbrk(-bytes) == (char *)-1)
    ok = 0;
  printf("snapshot: %s\n", ok ? "OK" : "FALLO");
  exit(ok ? 0 : 1);
}

static void
mapa(void)
{
  // Una tabla pequeña para que la salida de vmprint() sea corta.
  char *t = sbrk(2 * PGSIZE);
  if(t == (char *)-1)
    morir("sbrk() falló");
  t[0] = t[PGSIZE] = 1;
  printf("mapa: tabla en 0x%lx (2 páginas)\n", (uint64)t);
  int pid = fork();
  if(pid < 0)
    morir("fork() falló");
  if(pid == 0){
    printf("mapa: hijo antes de escribir\n");
    vmprint();
    t[0] = 2;
    printf("mapa: hijo después de escribir en 0x%lx\n", (uint64)t);
    vmprint();
    exit(0);
  }
  wait(0);
  exit(0);
}

int
main(int argc, char *argv[])
{
  if(argc >= 2 && strcmp(argv[1], "consulta") == 0)
    consulta(argc >= 3 ? atoi(argv[2]) : 1);
  if(argc >= 2 && strcmp(argv[1], "snapshot") == 0)
    snapshot(argc >= 3 ? atoi(argv[2]) : 2000);
  if(argc >= 2 && strcmp(argv[1], "mapa") == 0)
    mapa();
  printf("uso: minidb consulta K | minidb snapshot M | minidb mapa\n");
  exit(1);
}
