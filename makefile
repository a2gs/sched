include $(CODE_ROOT)/ARQUIVOS_MAKE/make.dirs

BIN_DIR=$(CUSTOM_BIN)

MORECCFLAGS=-DSYSV -DPROTOTYPE -DAIX -DDEV -DSYBASE_RPC -DRISC6000 -DIG_VERSION

INCLUDES=	-I. \
			-I$(DEFAULT_BASELINEDIR)/include  \
			-I$(DEFAULT_PREFIX)/include \
			-I$(IST_ROOT)/sql/$A \
			-I../../INCLUDES \
			-I../../../../$(SITE)/INCLUDES \
			-I../../INCLUDES


#CFLAGS= $(INCLUDES) -g  -qextchk -qfullpath -qflag=w:i $(MORECCFLAGS) -bnoquiet
CFLAGS= $(INCLUDES) -g  -qextchk -qfullpath -qflag=w:i $(MORECCFLAGS)

MATHLIBS = -lm

GENERIC_LIBS=-L$(DEFAULT_BASELINEDIR)/lib -loistsql -lomisc -losyslg -locfg -loargv0 -locatsig -lpthread

LIBRARIES=$(PRODUCT_LIBS) $(GENERIC_LIBS) $(SWITCH_LIBS) $(MATHLIBS)



all:	mynohup

mynohup:	mynohup.c
	cc -o mynohup mynohup.c $(CFLAGS) $(GENERIC_LIBS) $(LIBRARIES)
	cp schedule_bkp schedule

clean:
	-rm mynohup.o mynohup log out?_test
