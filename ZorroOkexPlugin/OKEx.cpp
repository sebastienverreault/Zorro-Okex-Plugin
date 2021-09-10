// OKEx broker plugin for Zorro ////////////////////////////
// All rights reserved
// Doc: https://www.okex.com/docs-v5/en

#include "pch.h"
#include <string>
#include <mmsystem.h>
#include <math.h>
#include <ATLComTime.h>

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	return TRUE;
}

/////////////////////////////////////////////////////////////
#pragma warning(disable : 4996 4244 4312)

// to use the Sleep function
#include <windows.h>		// Sleep(), in miliseconds
#include <time.h>

typedef double DATE;
//#include "..\include\trading.h"
#include "..\..\Zorro_2418Beta\include\trading.h"
#include "jsmn.h"
#include "sha256.h"
#include "base64.h"

//#define DEBUG(a,b) 
//#define DEBUG(a,b) showError(a,b);
#define PLUGIN_VERSION 2

#define DLLFUNC extern "C" __declspec(dllexport)
#define SAFE_RELEASE(p) if(p) p->release(); p = NULL

/////////////////////////////////////////////////////////////
const char* NAME = "OKEx V5";
const char* RHEADER = "https://www.okex.com/";
const char* TOKERR = "error";

//The Production Trading URL :
//REST: https://www.okex.com
//Public WebSocket: wss://ws.okex.com:8443/ws/v5/public
//Private WebSocket: wss://ws.okex.com:8443/ws/v5/private

//The Demo Trading URL :
//REST: https://www.okex.com
//Public WebSocket: wss://wspap.okex.com:8443/ws/v5/public?brokerId=9999
//Private WebSocket: wss://wspap.okex.com:8443/ws/v5/private?brokerId=9999
//Note: "x-simulated-trading: 1" needs to be added to the header of the Demo Trading request.

//Http Header Example 
//Content-Type: application/json
//OK-ACCESS-KEY: 37c541a1-****-****-****-10fe7a038418
//OK-ACCESS-SIGN: leaVRETrtaoEQ3yI9qEtI1CZ82ikZ4xSG5Kj8gnl3uw=
//OK-ACCESS-PASSPHRASE: 1****6
//OK-ACCESS-TIMESTAMP: 2020-03-28T12:21:41.274Z
//x-simulated-trading: 1


/////////////////////////////////////////////////////////////
int (__cdecl *BrokerError)(const char *txt) = NULL;
int (__cdecl *BrokerProgress)(const int percent) = NULL;
int (__cdecl *http_send)(const char* url, const char* data, const char* header) = NULL;
long (__cdecl *http_status)(int id) = NULL;
long (__cdecl *http_result)(int id,char* content,long size) = NULL;
int (__cdecl *http_free)(int id) = NULL;

static int loop_ms = 50, wait_ms = 30000;
static BOOL g_bDemoOnly = TRUE;
static BOOL g_bIsDemo = TRUE;
static BOOL g_bConnected = FALSE;
static char g_Account[32] = "", g_Asset[64] = "";
static char g_Uuid[256] = "";
static char g_Password[256] = "", g_Secret[256] = "", g_Passphrase[256] = "";
static char g_Command[1024];
static int g_Warned = 0;
static BOOL isForex;
__int64 g_IdOffs64 = 0;
static int g_HttpId = 0;
static int g_nDiag = 0;
static double g_Amount = 1.;
static double g_Limit = 0.;
static int g_VolType = 0;
static int g_Id = 0;
static int g_OrderType = 0;

#define OBJECTS	300
#define TOKENS		20+OBJECTS*32

void showError(const char* text,const char *detail)
{
	static char msg[4096];
	if(!detail) detail = "";
	sprintf_s(msg,"%s %s",text,detail);
//	if(strstr(msg,"imit viola")) return;
	BrokerError(msg);
/*	if(g_nDiag >= 1) {
		sprintf_s(msg,"Cmd: %s",g_Command);
		BrokerError(msg);
	}*/
}

char* itoa(int n)
{
	static char buffer[64];
	if(0 != _itoa_s(n,buffer,10)) *buffer = 0;
	return buffer;
}

char* i64toa(__int64 n)
{
	static char buffer[64];
	if(0 != _i64toa_s(n,buffer,64,10)) *buffer = 0;
	return buffer;
}


int atoix(char* c) { return (int)(_atoi64(c)-g_IdOffs64); }

char* ftoa(double f)
{
	static char buffer[64];
	if(f < 1.)
		sprintf(buffer,"%.8f",f);
	else if(f < 30.)
		sprintf(buffer,"%.4f",f);
	else if(f < 300) 
		sprintf(buffer,"%.2f",f); // avoid "invalid precision"
	else 
		sprintf(buffer,"%.0f",f);
	return buffer;
}

double roundto(var Val,var Step)
{
	return Step*(floor(Val/Step+0.5));
}

int sleep(int ms)
{
	Sleep(ms); 
	return BrokerProgress(0);
}

inline BOOL isConnected(int Key = 0)
{
	if(g_bDemoOnly || !g_bConnected) return 0;
	if(Key && (!*g_Password || !*g_Secret)) return 0;
	return 1;
}

DATE convertTime(__int64 t64)
{
	if(t64 == 0) return 0.;
	return (25569. + ((double)(t64/1000))/(24.*60.*60.));
}

__int64 convertTime(DATE Date)
{
	return 1000*(__int64)((Date - 25569.)*24.*60.*60.);
}



// convert ETH/BTC -> ethbtc
char* fixAsset(char* Asset,int Mode = 1)
{
	static char NewAsset[32];
	char* Minus = strchr(Asset,'-'); 
// convert BTC-ETH -> ethbtc
	if(Minus) {
		strcpy_s(NewAsset,Minus+1);
		strcat_s(NewAsset,Asset);
		Minus = strchr(NewAsset,'-'); 
		*Minus = 0;
	} else {
// convert ETH/BTC -> ETHBTC
		strcpy_s(NewAsset,Asset);
		char* Slash = strchr(NewAsset,'/'); 
		if(Slash) {
			if(Mode == 2)
				*Slash = 0; // -> ETH, for balance
			else
				strcpy_s(Slash,16,Slash+1);
		}
	}
//	if(Lwr) strlwr(NewAsset);
	return NewAsset;
}

const char* getSignature(std::string Post)
{
	std::string PrivateKey = g_Secret;

//Post = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
//PrivateKey = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
// -> c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71

	unsigned char hmac_256[SHA256::DIGEST_SIZE];
	memset(hmac_256, 0, SHA256::DIGEST_SIZE);
	//std::string encp = base64_decode(PrivateKey);
	HMAC256(PrivateKey, (unsigned char *)Post.c_str(), (int)Post.length(), hmac_256);

	static char buf[2 * SHA256::DIGEST_SIZE + 1];
	buf[2 * SHA256::DIGEST_SIZE] = 0;
	for (int i = 0; i < SHA256::DIGEST_SIZE; i++)
		sprintf(buf + i * 2, "%02x", hmac_256[i]);
	
	return buf;
}

static int DoTok = 0;

char* send(const char* dir,const char* param = NULL,int crypt = 0)
{
	int id;
	strcpy_s(g_Command,RHEADER);
	strcat_s(g_Command,dir);
	if(crypt) {
		int CmdLength = (int)strlen(g_Command);
		strcat_s(g_Command,"?recvWindow=50000");
		strcat_s(g_Command,"&timestamp=");
		__time64_t Time;
		_time64(&Time);
		strcat_s(g_Command,i64toa(Time*1000));
		if(param)
			strcat_s(g_Command,param);

		char* TotalParams = g_Command+CmdLength+1;
		//TotalParams = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
		const char* Signature = getSignature(TotalParams); 
		strcat_s(g_Command,"&signature=");
		strcat_s(g_Command,Signature);
		
		char Header[1024]; 
		//strcpy_s(Header,"Content-Type:application/json");
		//strcpy_s(Header,"Content-Type: application/json");
		//strcpy_s(Header,"OK-ACCESS-KEY: ");
		//strcat_s(Header, g_okAccessKey);					// 37c541a1-****-****-****-10fe7a038418
		//strcpy_s(Header,"OK-ACCESS-SIGN: ");
		//strcat_s(Header, g_okAccessSign);					// leaVRETrtaoEQ3yI9qEtI1CZ82ikZ4xSG5Kj8gnl3uw=
		//strcpy_s(Header,"OK-ACCESS-PASSPHRASE: ");
		//strcat_s(Header, g_okAccessPassphrase);			// 1****6
		//strcpy_s(Header,"OK-ACCESS-TIMESTAMP: ");
		//strcat_s(Header, g_okAccessTimestamp);			// 2020-03-28T12:21:41.274Z
		//strcpy_s(Header,"x-simulated-trading: 1");

		//strcat_s(Header,"\nAccept:application/json");
		strcpy_s(Header,"X-MBX-APIKEY: ");
		strcat_s(Header,g_Password);
		
		if(crypt == 3)
			id = http_send(g_Command,"#DELETE",Header);
		else if(crypt == 2)
			id = http_send(g_Command,"#POST",Header);
		else
			id = http_send(g_Command,NULL,Header);
	} else {
		if(param) 
			strcat_s(g_Command,param);
		id = http_send(g_Command,NULL,NULL);
	}

	if(crypt >= 2 && g_nDiag >= 2) {
		showError("Send:",g_Command);
	}
	if(!id) return NULL;
	DoTok = 1;	// tokenize again
	int len = http_status(id);
	for(int i=0; i<30*1000/20 && !len; i++) {
		sleep(20); // wait for the server to reply
		len = http_status(id);
	}
	if(len > 0) { //transfer successful?
		static char s[OBJECTS*512];
		int res = http_result(id,s,sizeof(s));
		s[sizeof(s)-1] = 0;
		if(!res || crypt)
			http_free(id);
		else
			g_HttpId = id;
		if(!res) return NULL;
		if(crypt >= 2 && g_nDiag >= 2) {
			showError("Result:",s);
		}
		return s;
	}
	http_free(id);
	g_HttpId = 0;
	return NULL;
}

const char* parse(char* str, const char* key=NULL)
{
	static char* json = str;
	static jsmn_parser jsmn;
	static jsmntok_t tok[TOKENS];
	static int nTok = 0;
	static int nNext = 0;
	if(str)
		nNext = 0;
	if(str && (DoTok || !key)) {	// tokenize the string
		jsmn_init(&jsmn);
		nTok = jsmn_parse(&jsmn,json,strlen(json),tok,TOKENS);
		if(nTok > 0) DoTok = 0;
		else return "";
	}
	if(key) { // find the correct token
		int KeyLen = (int)strlen(key);
		for(int i=nNext; i<nTok-1; i++) {
			if(tok[i].type == JSMN_STRING && 
				(tok[i+1].type == JSMN_PRIMITIVE || tok[i+1].type == JSMN_STRING)) 
			{
				char* name = json + tok[i].start;
				i++;
				if(0 == strncmp(name,key,KeyLen)) {
					char* s = json + tok[i].start;
					json[tok[i].end] = 0;
					nNext = i+1;	// continue with next token
					return s;
				}
			}
		}
	}
	return "";
}

////////////////////////////////////////////////////////////////

DLLFUNC int BrokerOpen(char* Name,FARPROC fpError,FARPROC fpProgress)
{
	//if(*Name == '4') {
	//	g_bDemoOnly = FALSE;
	//} else {
	//	g_bDemoOnly = TRUE;
	//}
	g_bDemoOnly = FALSE;
	strcpy_s(Name,32,NAME);
	(FARPROC&)BrokerError = fpError;
	(FARPROC&)BrokerProgress = fpProgress;
	return PLUGIN_VERSION;
}

DLLFUNC int BrokerHTTP(FARPROC fp_send,FARPROC fp_status,FARPROC fp_result,FARPROC fp_free)
{
	(FARPROC&)http_send = fp_send;
	(FARPROC&)http_status = fp_status;
	(FARPROC&)http_result = fp_result;
	(FARPROC&)http_free = fp_free;
	return 1;
}
////////////////////////////////////////////////////////////////

DLLFUNC int BrokerTime(DATE *pTimeGMT)
{
	if(!isConnected()) return 0;
	return 2;
}


DLLFUNC int BrokerAccount(char* Account,double *pdBalance,double *pdTradeVal,double *pdMarginVal)
{
	if(!isConnected(1)) return 0;

	if(!Account || !*Account) 
		Account = (char *)"BTC";

	char* Response = send("v3/account",0,1);
	if(!Response) return 0;
	parse(Response);
	double Balance = 0;
	while(1) {
		const char* Found = parse(NULL,"asset");
		if(!Found || !*Found) break;
		if(0 == strcmp(Found,Account))
			Balance += atof(parse(NULL,"free")); // deposit - exchange - trading
	}
	if(pdBalance) *pdBalance = Balance;
	return Balance > 0.? 1 : 0;
}


DLLFUNC int BrokerAsset(char* Asset,double* pPrice,double* pSpread,
	double *pVolume, double *pPip, double *pPipCost, double *pMinAmount,
	double *pMarginCost, double *pRollLong, double *pRollShort)
{
	if(!isConnected()) return 0;

	char* Result =	send("v3/ticker/bookTicker?symbol=",fixAsset(Asset),0);
	if(!Result || !*Result) return 0;
	if(!parse(Result)) return 0;
	double Bid = atof(parse(Result,"bidPrice"));
	double Ask = atof(parse(Result,"askPrice"));
	double Volume = atof(parse(Result,"bidQty"))+atof(parse(Result,"askQty"));
	if(Ask == 0. || Bid == 0.) return 0;
	if(pPrice) *pPrice = Ask;
	if(pSpread) *pSpread = Ask - Bid;
	if(pVolume) *pVolume = Volume;
	return 1;
}	


DLLFUNC int BrokerHistory2(char* Asset,DATE tStart,DATE tEnd,int nTickMinutes,int nTicks,T6* ticks)
{
	if(!isConnected()) return 0;
	if(!ticks || !nTicks) return 0;

	const char *tf = "1m";
	if(1440 <= nTickMinutes) tf = "1d"; 
	else if(60 <= nTickMinutes) tf = "1h"; 
	else if(30 <= nTickMinutes) tf = "30m";
	else if(15 <= nTickMinutes) tf = "15m";
	else if(5 <= nTickMinutes) tf = "5m"; 
	
	char Command[256] = "?symbol=";
	strcat_s(Command,fixAsset(Asset,0));
	strcat_s(Command,"&interval=");
	strcat_s(Command,tf);
	strcat_s(Command,"&limit=");
	strcat_s(Command,itoa(nTicks));
	strcat_s(Command,"&startTime=");
	strcat_s(Command,i64toa(convertTime(tStart)));
	strcat_s(Command,"&endTime=");
	strcat_s(Command,i64toa(convertTime(tEnd)));
	char* Result = send("v1/klines",Command,0);
	if(!Result || !*Result) goto raus;
	Result = strchr(Result,'[');
	if(!Result || !*Result) goto raus;
	{
		int i = 0;
		for (; i < nTicks; i++, ticks++) {
			Result = strchr(++Result, '[');
			if (!Result || !*Result) break;
			__int64 TimeOpen, TimeClose;
			sscanf(Result, "[%I64d,\"%f\",\"%f\",\"%f\",\"%f\",\"%f\",%I64d,",
				&TimeOpen, &ticks->fOpen, &ticks->fHigh, &ticks->fLow, &ticks->fClose, &ticks->fVol, &TimeClose);
			ticks->time = convertTime(TimeClose);
		}
		return i;
	}
raus:
	if(g_Warned++ <= 1) showError(Asset,"no data");
	return 0;
}



// returns negative amount when the trade was closed
DLLFUNC int BrokerTrade(int nTradeID,double *pOpen,double *pClose,double *pRoll,double *pProfit)
{
	if(!isConnected(1)) return 0;

	char Param[512] = "&symbol=";
	strcat_s(Param,g_Asset);
	strcat_s(Param,"&origClientOrderId=");
	strcat_s(Param,itoa(nTradeID));
	char* Result = send("v3/order",Param,1);
	if(!Result || !*Result) return 0;
/*// response
{
  "symbol": "LTCBTC",
  "orderId": 1,
  "clientOrderId": "myOrder1",
  "price": "0.1",
  "origQty": "1.0",
  "executedQty": "0.0",
  "status": "NEW",
  "timeInForce": "GTC",
  "type": "LIMIT",
  "side": "BUY",
  "stopPrice": "0.0",
  "icebergQty": "0.0",
  "time": 1499827319559,
  "isWorking": true
 }*/
	if(!strstr(Result,"clientOrderId")) return 0;
	if(!parse(Result)) return 0;
	double Price = atof(parse(Result,"price"));
	if(pOpen) *pOpen = Price;
	int Fill = atof(parse(Result,"executedQty"))/g_Amount;
	return Fill;
}

DLLFUNC int BrokerBuy2(char* Asset,int Amount,double dStopDist,double Limit,double *pPrice,int *pFill)
{
	if(!isConnected(1)) return 0;
	strcpy_s(g_Asset,fixAsset(Asset)); // for BrokerTrade

	char Param[512] = "&symbol=";
	strcat_s(Param,g_Asset);
	strcat_s(Param,"&side=");
	if(Amount > 0) 
		strcat_s(Param,"BUY");
	else
		strcat_s(Param,"SELL");
	strcat_s(Param,"&type=");
	if(Limit > 0.) {
		double TickSize = 0.0000001;
		Limit = ((int)(Limit/TickSize))*TickSize;	// clip to integer multiple of tick size
		strcat_s(Param,"LIMIT");
		strcat_s(Param,"&price=");
		strcat_s(Param,ftoa(Limit));
		strcat_s(Param,"&timeInForce=");
		if(g_OrderType == 1)
			strcat_s(Param,"IOC");
		else if(g_OrderType == 2)
			strcat_s(Param,"GTC");
		else
			strcat_s(Param,"FOK");
	} else
		strcat_s(Param,"MARKET");
	strcat_s(Param,"&newOrderRespType=FULL");
	strcat_s(Param,"&quantity=");
	strcat_s(Param,ftoa(g_Amount*labs(Amount)));
	strcat_s(Param,"&newClientOrderId=");
	strcat_s(Param,itoa(g_Id++));
	char* Result = send("v3/order",Param,2);
	if(!Result || !*Result) {
		showError(Param,"- no result");
		return 0;
	}
/*// response
{
  "symbol": "BTCUSDT",
  "orderId": 28,
  "clientOrderId": "6gCrw2kRUAF9CvJDGP16IP",
  "transactTime": 1507725176595,
  "price": "0.00000000",
  "origQty": "10.00000000",
  "executedQty": "10.00000000",
  "status": "FILLED",
  "timeInForce": "GTC",
  "type": "MARKET",
  "side": "SELL"
 }*/
	if(!strstr(Result,"clientOrderId")) {
		showError(Param,Result);
		return 0;
	}
	if(!parse(Result)) {
		showError(Result,"- invalid");
		return 0;
	}
	int Id = atoi(parse(Result,"clientOrderId"));
	int Fill = atof(parse(Result,"executedQty"))/g_Amount;
	parse(NULL,"fills");
	double Price = atof(parse(NULL,"price"));
	double Commission = atof(parse(NULL,"commission"));
	if(pPrice && Price > 0.) *pPrice = Price;
	if(pFill) *pFill = Fill;
	if(g_OrderType == 2 || Fill == Amount)
		return Id; // fully filled or GTC
	if(Fill)
		return Id; // IOC partially or FOK fully filled
	return 0;
}


DLLFUNC int BrokerLogin(char* User,char* Pwd,char* Type,char* Account)
{
	if(User) {
		//if(g_bDemoOnly) {
		//	showError("Need Zorro S for OKEx","");
		//	return 0;
		//}

		if (0 == strcmp(Type, "Demo")) 
			g_bIsDemo = TRUE;
		else
			g_bIsDemo = FALSE;

		g_Warned = 0;
		strcpy_s(g_Password,User);
		strcpy_s(g_Secret, Pwd);
		char* Space = strchr(g_Secret, ' ');
		if (Space)
		{
			strcpy_s(g_Passphrase, Space + 1);
			*Space = 0;
		}
		else {
			*g_Password = 0;
			*g_Secret = 0;
			BrokerError("Error: Password must be divisible \n\ninto two space-separated strings.\n\nThe password should be formed like so:\n '<SecretKey> <Passphrase>' \n\nwithout quotes.");
			return 0;
		}

		g_bConnected = 1;
		time_t Time;
		time(&Time);
		g_Id = (int)Time;
		if(!*User || !*Pwd) {
			showError("Price data only","");
		} else 
			/*if(!BrokerAccount(Account,NULL,NULL,NULL))
			return 0;*/
		return 1;
	} else {
		if(g_HttpId) 
			http_free(g_HttpId);
		g_HttpId = 0;
		*g_Password = 0;
		*g_Secret = 0;
	}
	return 0;
}

//////////////////////////////////////////////////////////////
DLLFUNC double BrokerCommand(int command,DWORD parameter)
{
	switch(command) {
		case SET_DELAY: loop_ms = parameter;
		case GET_DELAY: return loop_ms;
		case SET_WAIT: wait_ms = parameter;
		case GET_WAIT: return wait_ms;
		case GET_COMPLIANCE: return 2;
		case SET_DIAGNOSTICS: g_nDiag = parameter; return 1;

		//case SET_LIMIT: g_Limit = *(double*)parameter; return 1;
		case SET_AMOUNT: g_Amount = *(double*)parameter; return 1;
		case GET_MAXREQUESTS: return 10;
		case GET_MAXTICKS: return 500;
		case SET_ORDERTYPE: 
			return g_OrderType = parameter;

		case SET_SYMBOL: { 
			char* Asset = fixAsset((char*)parameter);
			if(!Asset || !*Asset) return 0;
			strcpy_s(g_Asset,Asset); 
			return 1;
		}

		case GET_POSITION: {
			double Balance = 0;
			char* Asset = fixAsset((char*)parameter);
			int Len = (int)strlen(Asset);
			if(Len > 5) Asset[Len-3] = 0; // clip trailing "BTC"
			BrokerAccount(Asset,&Balance,NULL,NULL);
			return Balance;
		}

		case DO_CANCEL: {
			if(!isConnected(1)) return 0;
			char Param[512] = "&symbol=";
			strcat_s(Param,g_Asset);
			strcat_s(Param,"&origClientOrderId=");
			strcat_s(Param,itoa(parameter));
			char* Result = send("v3/order",Param,3);
			if(!Result || !*Result) return 0;
			return 1;
		}

		case GET_BOOK: {
/*
			T2* Quotes = (T2*)parameter;
			if(!Quotes) return 0;
			char* Result =	send("public/getmarketsummary?market=",g_Asset,1);
			if(Result && *Result)
				Quotes[0].time = convertTime(parse(Result,"TimeStamp"));
			else
				return 0;
			int N = 0;
			Result = send("public/getorderbook?type=sell&market=",g_Asset,0);
			if(!Result) return N;
			char* Success = parse(Result,"success");
			if(!strstr(Success,"true")) return N;
			for(; N<MAX_QUOTES/2; N++) {
				Quotes[N].fVol = atof(parse(NULL,"Quantity"));
				Quotes[N].fVal = atof(parse(NULL,"Rate"));	// ask
				if(Quotes[N].fVal == 0.) 
					break;
				Quotes[N].time = Quotes[0].time;
			}
			Result = send("public/getorderbook?type=buy&market=",g_Asset,0);
			if(!Result) return N;
			Success = parse(Result,"success");
			if(!strstr(Success,"true")) return N;
			for(; N<MAX_QUOTES-1; N++) {
				Quotes[N].fVol = atof(parse(NULL,"Quantity"));
				Quotes[N].fVal = -atof(parse(NULL,"Rate"));	// bid
				if(Quotes[N].fVal == 0.) 
					break;
				Quotes[N].time = Quotes[0].time;
			}
			Quotes[N].fVol = Quotes[N].fVal = 0.f;
			Quotes[N].time = 0.; // end mark
			return N;
*/
			return 0;
		}
	}

	return 0.;
}

