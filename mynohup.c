/*
 * mynohup
 * Um nohup com cron simplificado
 *
 * a2gs
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ==================================================================================================== */

#define Y    (1)
#define N    (0)
#define OK   (1)
#define ERRO (0)

#define DEBUG_FILE ("~/mynohup/log")
#define SCHED_FILE ("~/mynohup/schedule")

#define CMD_SIZE     (1024)
#define TOTAL_SCHED  (30)
#define SLEEP_TIME   (10)     /* segundos */
#define LOGBUF_SZ    (2048)
#define SCHEDLINE_SZ (1024)
#define PATH_SZ      (512)

/* ==================================================================================================== */

typedef enum{
	HORARIA = 1,
	DIARIA,
	MENSAL,
	EVENTUAL
}recursivo_t;

typedef enum{
	LIVRE_PARA_EXECUCAO = 1,
	EXECUTANDO,
	FINALIZADO
}proc_status_t;

typedef struct _sched_t{

	/* Comando */
	char cmd[CMD_SIZE + 1];

	/* Indica o status do processo */
	proc_status_t status;

	/* Posicao do registro no arquivo de schedule */
	long offset;

	/* Data que esta registrado para iniciar o comando */
	unsigned int ult_exec_ano, ult_exec_mes, ult_exec_dia, ult_exec_hora, ult_exec_min, ult_exec_seg;

	/* Periodo de recursividade */
	recursivo_t rec;

}sched_t;

/* ==================================================================================================== */

FILE *debugfile = NULL; /* Arquivo de Log */
FILE *schedfile = NULL; /* Arquivo schedule */

sched_t scheduler[TOTAL_SCHED] = {0};

pthread_mutexattr_t mattr;
pthread_mutex_t mp = PTHREAD_MUTEX_INITIALIZER;

struct tm agora = {0};
time_t agora_time = 0;

/* ==================================================================================================== */
	
int log(char *msg, ...)
{
	int retPrint = 0;
	struct tm logTimeTm = {0};
	time_t logTimeTimet = 0;
	va_list args  = {0};
	struct timeval tv = {0};
	struct timezone tz = {0};
	char logbuff[LOGBUF_SZ + 1] = {0};

	time(&logTimeTimet);
	memcpy(&logTimeTm, localtime(&logTimeTimet), sizeof(struct tm));
	strftime(logbuff, LOGBUF_SZ, "%Y%m%d %H%M%S", &logTimeTm);

	if(gettimeofday(&tv, &tz) == 0){
		sprintf(&logbuff[15], " %07ld|", tv.tv_usec);
	}

	sprintf(&logbuff[24], "%d %d|", getpid(), getppid());

	retPrint = strlen(logbuff);

	va_start(args, msg);

	retPrint += vsnprintf(&logbuff[retPrint], LOGBUF_SZ - retPrint, msg, args);

	va_end(args);

	logbuff[retPrint++] = '\n';

	fwrite(logbuff, retPrint, 1, debugfile);

	return(ferror(debugfile) == 0 ? OK : ERRO);
}

/* ==================================================================================================== */

/*
 * Funcoes que retornam ou traduzem periodicidade.
 */

proc_status_t getPeridicidade(char s)
{
	if(s == 'M' || s == 'm')                  return(MENSAL);
	else if(s == 'D' || s == 'd')             return(DIARIA);
	else if(s == 'H' || s == 'h')             return(HORARIA);
	else if(s == 'E' || s == 'e' || s == '*') return(EVENTUAL);
	else                                      return(EVENTUAL);
}

char translatePeridicidade(proc_status_t s)
{
	if(s == MENSAL)        return('M');
	else if(s == DIARIA)   return('D');
	else if(s == HORARIA)  return('H');
	else if(s == EVENTUAL) return('E');
	else                   return('E');
}

/* ==================================================================================================== */

/*
 * Funcoes que retornam ou traduzem status.
 */

proc_status_t getStatus(char s)
{
	if(s == 'I' || s == 'i')      return(LIVRE_PARA_EXECUCAO);
	else if(s == 'E' || s == 'e') return(EXECUTANDO);
	else if(s == 'F' || s == 'f') return(FINALIZADO);
	else                          return(FINALIZADO);
}

char translateStatus(proc_status_t s)
{
	if(s == LIVRE_PARA_EXECUCAO) return('I');
	else if(s == EXECUTANDO)     return('E');
	else if(s == FINALIZADO)     return('F');
	else                         return('F');
}

/* ==================================================================================================== */

/*
 * Le o arquivo SCHED_FILE do inicio ao fim e carrega na memoria.
 */

int loadSchedArqToMem(sched_t *sched, size_t sched_sz)
{
	char linha[(SCHEDLINE_SZ + 1) < (CMD_SIZE + 1) ? CMD_SIZE : SCHEDLINE_SZ] = {0};
	char aux[20] = {0}, *c = NULL;
	int i = 0;

	pthread_mutex_lock(&mp);

	/* O arquivo de schedule ja deve estar aberto */
	if(schedfile == NULL){
		log("Erro em abrir arquivo de schedule [%s]: [%s]", SCHED_FILE, strerror(errno));
		pthread_mutex_unlock(&mp);
		return(ERRO);
	}

	rewind(schedfile);

	/*
	FORMATO DO ARQUIVO SCHEDULE:

	0         1         2         3         4
	012345678901234567890123456789012345678901234567890

	YYYYMMDD HHMMSS ZX CMD
	20101220-130000 #* clear

	YYYYMMDD HHMMSS - Data e hora da primeira execucao
	Z - Status do comando
	X - Tipo de periodicidade
	CMD - Comando
	*/

	i = 0;

	while(!feof(schedfile)){

		sched[i].offset = ftell(schedfile);
		if(sched[i].offset == -1){
			log("Erro de offset no arquivo de schedule [%s]: [%s]", SCHED_FILE, strerror(errno));
			pthread_mutex_unlock(&mp);
			return(ERRO);
		}

		memset(linha, 0, SCHEDLINE_SZ + 1);
		fgets(linha, SCHEDLINE_SZ, schedfile);

		if(linha[0] == '#' || linha[0] == '\t' || linha[0] == ' ' || linha[0] == '\0' || linha[0] == '\n') continue;

		/* Comandos com status FINALIZADO nao serao mais carregados */
		if(linha[32] == 'F' || linha[32] == 'f') continue;

		/* Ano ultima execucao */
		strncpy(aux, &linha[0], 4); aux[4] = '\0';
		sched[i].ult_exec_ano = atoi(aux);

		/* Mes ultima execucao */
		strncpy(aux, &linha[4], 2); aux[2] = '\0';
		sched[i].ult_exec_mes = atoi(aux);

		/* Dia ultima execucao */
		strncpy(aux, &linha[6], 2); aux[2] = '\0';
		sched[i].ult_exec_dia = atoi(aux);

		/* Hora ultima execucao */
		strncpy(aux, &linha[9], 2); aux[2] = '\0';
		sched[i].ult_exec_hora = atoi(aux);

		/* Minuto ultima execucao */
		strncpy(aux, &linha[11], 2); aux[2] = '\0';
		sched[i].ult_exec_min = atoi(aux);

		/* Segundo ultima execucao */
		strncpy(aux, &linha[13], 2); aux[2] = '\0';
		sched[i].ult_exec_seg = atoi(aux);

		/* Status do comando */
		sched[i].status = getStatus(linha[16]);

		/* Periodicidade */
		sched[i].rec = getPeridicidade(linha[17]);

		/* Comando */
		strcpy(sched[i].cmd, &linha[19]);
		c = strchr(sched[i].cmd, '\n');
		if(c != NULL) *c = '\0';

		i++;
	}

	pthread_mutex_unlock(&mp);

	return(OK);
}

/* ==================================================================================================== */

/*
 * Imprime no arquivo de debug toda a lista de schedule em memoria.
 */

void printScheduleMemory(sched_t *sc, size_t sz)
{
	unsigned int i = 0;

	for(i = 0; (i < sz) && (sc[i].cmd[0] != '\0'); i++)
		log("[%04d/%02d/%02d %02d:%02d:%02d %c%c %s]",
		    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
		    sc[i].ult_exec_min, sc[i].ult_exec_seg, translateStatus(sc[i].status),
		    translatePeridicidade(sc[i].rec), sc[i].cmd);

	return;
}

/* ==================================================================================================== */

void mynohup_exit(int status)
{

	log("Sinal [%d] recebido. Terminando...", status);

	mynohup_end();
	exit(0);
}

/* ==================================================================================================== */

/*
 * Inicializacao do sistema:
 * - Abre arquivo de debug
 * - Abre arquivo SCHED_FILE
 * - Mutex de acesso ao arquivo SCHED_FILE
 */

int mynohup_init(void)
{
	int ret = 0;
	uid_t uid = 0;
	struct passwd *pw = NULL;
	char path[PATH_SZ + 1] = {0};

	uid = geteuid();
	pw  = getpwuid(uid);
	getcwd(path, PATH_SZ);

	debugfile = fopen(DEBUG_FILE, "a");
	if(debugfile == NULL){
		fprintf(stderr, "ERRO: Nao foi possivel abrir arquivo de debug [%s]: [%s].\n", DEBUG_FILE, strerror(errno));
		return(ERRO);
	}
	setvbuf(debugfile, (char *)NULL, _IOLBF, 0);

	/* Mutex do arquivo scheduler */
	ret = pthread_mutexattr_init(&mattr);
	if(ret != 0){
		log("Erro em criar mutex: [%s]", strerror(ret));
		return(ERRO);
	}

	ret = pthread_mutex_init(&mp, &mattr);
	if(pthread_mutex_init(&mp, &mattr) != 0){
		log("Erro em inicializar mutex: [%s]", strerror(ret));
		return(ERRO);
	}

	schedfile = fopen(SCHED_FILE, "r+");
	if(schedfile == NULL){
	   log("Erro em abrir arquivo de schedule [%s]: [%s]", SCHED_FILE, strerror(errno));
	   return(ERRO);
	}

	log("Sistema inicializado");
	log("Usuario efetivo: [%s] eUID: [%d] Home: [%s]", pw->pw_name, (unsigned long)getpwuid(getuid())->pw_uid, pw->pw_dir);
	log("Path corrente: [%s]", path);

	return(OK);
}

/* ==================================================================================================== */

/*
 * Finalizacao do sistema:
 * - Fecha arquivo de debug
 * - Fecha arquivo SCHED_FILE
 * - Destriu mutex de acesso ao arquivo SCHED_FILE
 */

int mynohup_end(void)
{
	int ret = 0;

	fflush(schedfile);
	fclose(schedfile);

	/* Destruindo os mutex'es */
	ret = pthread_mutexattr_destroy(&mattr);
	if(ret != 0){
		fprintf(stderr, "Erro em destruir atributos do mutex: [%s]", strerror(ret));
		return(ERRO);
	}

	ret = pthread_mutex_destroy(&mp);
	if(ret != 0){
		fprintf(stderr, "Erro em destruir mutex: [%s]", strerror(ret));
		return(ERRO);
	}

	fclose(debugfile);

	return(OK);
}

/* ==================================================================================================== */

void reload(int sig)
{
	catch_all_signals(mynohup_exit);
	signal(SIGUSR1, reload);
	signal(SIGALRM1, SIG_DFL);

	if(loadSchedArqToMem(scheduler, TOTAL_SCHED) != OK){
		log("Erro em atualizar scheduler em memoria com arquivo [%s]!", SCHED_FILE);
	}

	printScheduleMemory(scheduler, TOTAL_SCHED);

	return;
}

/* ==================================================================================================== */

/*
 * Grava no arquivo SCHED_FILE os dados apontados em *sc (utiliza a varaivel offset da struct sched_t
 * para acessar a linha do registro no arquivo SCHED_FILE).
 */

int atualiza_sched(sched_t *sc)
{
	char status = 0;
	int ret = 0;

	status = translateStatus(sc->status);

	log("Atualizando status do comando [%s] para [%c]", sc->cmd, status);

	pthread_mutex_lock(&mp);

	if(fseek(schedfile, sc->offset, SEEK_SET) == -1){
		log("Erro em atualizar registro [%s]: [%s]!", sc->cmd, strerror(errno));
		pthread_mutex_unlock(&mp);
		return(ERRO);
	}

	ret = fprintf(schedfile, "%04d%02d%02d-%02d%02d%02d %c",
	        sc->ult_exec_ano, sc->ult_exec_mes, sc->ult_exec_dia, sc->ult_exec_hora,
	        sc->ult_exec_min, sc->ult_exec_seg, status);

	fflush(schedfile);

	pthread_mutex_unlock(&mp);

	return(OK);
}

/* ==================================================================================================== */

/*
 * Executa o comando passado como parametro e marca o registro como EXECUTANDO ou FINALIZADO.
 */

void * executa_cmd(void *data)
{
	int ret = 0;
	sched_t *sc;

	sc = (sched_t *)data;

	sc->status = EXECUTANDO;
	if(atualiza_sched(sc) != OK){
		log("Erro atualizando schedule [%s] com status EXECUTANDO", sc->cmd);
	}

	ret = system(sc->cmd);
	log("COMANDO [%s] EXECUTADO", sc->cmd);

	sc->status = FINALIZADO;
	if(atualiza_sched(sc) != OK){
		log("Erro atualizando schedule [%s] com status FINALIZADO", sc->cmd);
	}

	pthread_exit((void *)OK);
}

/* ==================================================================================================== */

/*
 * Verifica se o schedule *sc deve ser executado ou nao como mensal
 */

int processa_mensal(sched_t *sc)
{
	int ret = 0;
	pthread_t thread_id;

	if(sc->ult_exec_ano      <= agora.tm_year ||
	   sc->ult_exec_mes      <= agora.tm_mon  ||
	   sc->ult_exec_dia + 30 <= agora.tm_mday){

		log("Processo mensal que deve ser executado [%s] status [%s]", sc->cmd, translateStatus(sc->status));

		ret = pthread_create(&thread_id, NULL, &executa_cmd, sc);
		if(ret != 0){
			log("Erro criando thread para execucao de [%s]", strerror(ret));
			return(ERRO);
		}
	}

	return(OK);
}

/* ==================================================================================================== */

/*
 * Verifica se o schedule *sc deve ser executado ou nao como diaria
 */

int processa_diaria(sched_t *sc)
{
	int ret = 0;
	pthread_t thread_id;

	if(sc->ult_exec_ano       <= agora.tm_year ||
	   sc->ult_exec_mes       <= agora.tm_mon  ||
	   sc->ult_exec_dia       <= agora.tm_mday ||
	   sc->ult_exec_hora + 24 <= agora.tm_mday){

		log("Processo diario que deve ser executado [%s] status [%s]", sc->cmd, translateStatus(sc->status));

		ret = pthread_create(&thread_id, NULL, &executa_cmd, sc);
		if(ret != 0){
			log("Erro criando thread para execucao de [%s]", strerror(ret));
			return(ERRO);
		}
	}

	return(OK);
}

/* ==================================================================================================== */

/*
 * Verifica se o schedule *sc deve ser executado ou nao como horaria
 */

int processa_horaria(sched_t *sc)
{
	int ret = 0;
	pthread_t thread_id;

	if(sc->ult_exec_ano      <= agora.tm_year ||
	   sc->ult_exec_mes      <= agora.tm_mon  ||
	   sc->ult_exec_dia      <= agora.tm_mday ||
	   sc->ult_exec_hora     <= agora.tm_mday ||
	   sc->ult_exec_min + 60 <= agora.tm_mday){

		log("Processo horario que deve ser executado [%s] status [%s]", sc->cmd, translateStatus(sc->status));

		ret = pthread_create(&thread_id, NULL, &executa_cmd, sc);
		if(ret != 0){
			log("Erro criando thread para execucao de [%s]", strerror(ret));
			return(ERRO);
		}
	}

	return(OK);
}

/* ==================================================================================================== */

/*
 * Executa schedule *sc como eventual
 */

int processa_eventual(sched_t *sc)
{
	int ret = 0;
	pthread_t thread_id;

	if(sc->ult_exec_ano  <= agora.tm_year ||
	   sc->ult_exec_mes  <= agora.tm_mon  ||
	   sc->ult_exec_dia  <= agora.tm_mday ||
	   sc->ult_exec_hora <= agora.tm_mday ||
	   sc->ult_exec_min  <= agora.tm_mday){

		log("Processo eventual que deve ser executado [%s] status [%s]", sc->cmd, translateStatus(sc->status));

		ret = pthread_create(&thread_id, NULL, &executa_cmd, sc);
		if(ret != 0){
			log("Erro criando thread para execucao de [%s]", strerror(ret));
			return(ERRO);
		}
	}

	return(OK);
}

/* ==================================================================================================== */

/*
 * Percorre a lista de schedulers na memoria (com status LIVRE_PARA_EXECUCAO), passando cada
 * registro para a devida funcao de periodicidade, cuja ira verificar se deve ou nao executar.
 */

int processa_scheduler(sched_t *sc, size_t sz)
{
	unsigned int i = 0;

   for(i = 0; (i < sz) && (sc[i].cmd[0] != '\0') && (sc[i].status == LIVRE_PARA_EXECUCAO); i++){

		switch(sc[i].rec){
			case MENSAL:
				if(processa_mensal(&sc[i]) != OK){
					log("Erro na execucao do comando: [%04d/%02d/%02d %02d:%02d:%02d %d %s]",
					    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
					    sc[i].ult_exec_min, sc[i].ult_exec_seg, sc[i].rec, sc[i].cmd);
				}else{
				}

				break;

			case DIARIA:
				if(processa_diaria(&sc[i]) != OK){
					log("Erro na execucao do comando: [%04d/%02d/%02d %02d:%02d:%02d %d %s]",
					    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
					    sc[i].ult_exec_min, sc[i].ult_exec_seg, sc[i].rec, sc[i].cmd);
				}else{
				}

				break;

			case HORARIA:
				if(processa_horaria(&sc[i]) != OK){
					log("Erro na execucao do comando: [%04d/%02d/%02d %02d:%02d:%02d %d %s]",
					    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
					    sc[i].ult_exec_min, sc[i].ult_exec_seg, sc[i].rec, sc[i].cmd);
				}else{
				}

				break;

			case EVENTUAL:
				if(processa_eventual(&sc[i]) != OK){
					log("Erro na execucao do comando: [%04d/%02d/%02d %02d:%02d:%02d %d %s]",
					    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
					    sc[i].ult_exec_min, sc[i].ult_exec_seg, sc[i].rec, sc[i].cmd);
				}else{
				}

				break;

			default:
				log("Peridiocidade do comando nao reconhecida: [%04d/%02d/%02d %02d:%02d:%02d %d %s]",
				    sc[i].ult_exec_ano, sc[i].ult_exec_mes, sc[i].ult_exec_dia, sc[i].ult_exec_hora,
				    sc[i].ult_exec_min, sc[i].ult_exec_seg, sc[i].rec, sc[i].cmd);
				break;
		}

	}

   return(OK);
}

/* ==================================================================================================== */

int main(int argc, char *argv[])
{
	argv0 = *argv;
	catch_all_signals(mynohup_exit);
	signal(SIGUSR1, reload);
	signal(SIGALRM1, SIG_DFL);

	if(mynohup_init() != OK){
		return(-1);
	}

	/* Carregando arquivo schedule para memoria */
	if(loadSchedArqToMem(scheduler, TOTAL_SCHED) == ERRO){
		log("Erro em carregar scheduler em memoria. Terminando...");
		return(-2);
	}

	log("Comandos agendados:", getuid());
	printScheduleMemory(scheduler, TOTAL_SCHED);

	for(; ; ){
		time(&agora_time);
		memcpy(&agora, localtime(&agora_time), sizeof(struct tm));

		if(processa_scheduler(scheduler, TOTAL_SCHED) != OK){
			log("Erro em processar a lista de schedulers! Terminando...");
			return(-3);
		}

		sleep(SLEEP_TIME);
	}

	mynohup_end();

	return(0);
}
