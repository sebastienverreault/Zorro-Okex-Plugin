// OKEx broker plugin for Zorro ////////////////////////////
// All rights reserved
// Doc: https://www.okex.com/docs-v5/en

#include "pch.h"
#include <string>
#include <mmsystem.h>
#include <math.h>
#include <ATLComTime.h>

#define SET_MARGIN_MODE			2000 // User supplied command with a single numerical parameter. //  (0:cross , 1:isolated, 2:cash)
#define SET_ACCOUNT_LEVERAGE	3000 // User supplied command with an array of 8 var parameters. //  ([0]:instId, [1]:lever, [2]:mgnMode -isolated | cross- [3]:posSide -long | short | net)
#define SET_TRADE_MODE			4000 // User supplied command with a text string.                //  (0:cross , 1:isolated, 2:cash)
#define SET_POSITION_MODE		4001 // User supplied command with a text string.                //  (long_short_mode | net_mode)

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
const char* HOST = "https://www.okex.com";
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
static char g_TradeMode[32] = "";
static char g_PositionMode[32] = "";
static char g_Uuid[256] = "";
static char g_Password[256] = "", g_Secret[256] = "", g_Passphrase[256] = "";
static char g_Timestamp[64];
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

BOOL isIndexAsset(char* Asset)
{
	// OKEx has USDT spot but not USD
	// so instrument of the "<XXX>-USD" are underlying indexes
	// to be able to get market data, 
	// we need to be able to identify them
	// and we need to use a different endpoint at minimum
	static char Usd[32];
	char* Minus = strchr(Asset, '-');
	if (Minus) {
		strcpy_s(Usd, Minus + 1);
		BOOL isIndex = (0 == strcmp(Usd, "USD"));
		//if(!isIndex) isIndex = (0 == strcmp(Usd, "USDT"));
		return isIndex;
	}
	return FALSE;
}

const char* getSignature(std::string Post)
{
	static char cstr[128];
	std::string PrivateKey = g_Secret;

	//Post = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
	//PrivateKey = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
	// -> c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71

	// Post = "2021-09-11T09:13:18.124ZGET/api/v5/account/balance"
	// PrivateKey = "84e00892-e563-4791-8bc8-4b624b1b8481"
	// => 
	// base64.c_str() == "PYWLHcBhpi2xUTzazoRjvVf8q2oq492Ym28mvd8/qKI="

	unsigned char hmac_256[SHA256::DIGEST_SIZE];
	memset(hmac_256, 0, SHA256::DIGEST_SIZE);
	HMAC256(PrivateKey, (unsigned char *)Post.c_str(), (int)Post.length(), hmac_256);
	std::string base64 = base64_encode(hmac_256, SHA256::DIGEST_SIZE);
	strcpy_s(cstr, base64.c_str());

	return cstr;
}

static int DoTok = 0;

char* send(const char* path, const char* body = NULL, int crypt = 0)
{
	int id;
	char Prehash[1024];
	strcpy_s(g_Command,HOST);
	strcat_s(g_Command,path);
	if(crypt) {
		// Timestamp the signature
		time_t Time;
		time(&Time);
		tm* tm;
		tm = gmtime(&Time);
		strftime(g_Timestamp, 64, "%Y-%m-%dT%H:%M:%SZ", tm);// 2021-09-10T13:53:46.123Z

		strcpy_s(Prehash, g_Timestamp);
		if (body)
			strcat_s(Prehash, "POST");
		else
			strcat_s(Prehash, "GET");
		strcat_s(Prehash, path);
		if(body) 
			strcat_s(Prehash, body);
		// Prehash = timestamp + "GET" + "/users/self/verify" + ""
		// "2021-09-11T09:13:18.124Z"
		// "GET"
		// "/api/v5/account/balance"
		// Prehash = "2021-09-11T09:13:18.124ZGET/api/v5/account/balance"
		// OK-ACCESS-KEY: "84e00892-e563-4791-8bc8-4b624b1b8481"
		// =>
		// OK-ACCESS-SIGN: "PYWLHcBhpi2xUTzazoRjvVf8q2oq492Ym28mvd8/qKI="
		const char* okexSignatureTest = getSignature("2021-09-11T09:13:18.124ZGET/api/v5/account/balance");
		const char* okexSignature = getSignature(Prehash);

		char Header[1024]; 
		strcpy_s(Header,"Content-Type: application/json");
		strcat_s(Header,"\nOK-ACCESS-KEY: "); strcat_s(Header, g_Password);
		strcat_s(Header,"\nOK-ACCESS-SIGN: "); strcat_s(Header, okexSignature);
		strcat_s(Header,"\nOK-ACCESS-PASSPHRASE: "); strcat_s(Header, g_Passphrase);
		strcat_s(Header,"\nOK-ACCESS-TIMESTAMP: "); strcat_s(Header, g_Timestamp);			
		if(g_bIsDemo) strcat_s(Header,"\nx-simulated-trading: 1");
		
		if(crypt == 3)
			id = http_send(g_Command,"#DELETE",Header);
		else if(crypt == 2)
			id = http_send(g_Command,"#POST",Header);
		else
			id = http_send(g_Command,body,Header);
	} else {
		id = http_send(g_Command,body,NULL);
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


BOOL isResponseOk(const char* path, const char* body, char* response, const char** names, const char** values, int nameValuePairCount)
{
	char Request[512];
	strcpy_s(Request, path);
	if (body) { 
		strcat_s(Request, " - "); 
		strcat_s(Request, body); 
	}

	// Validate there's a response to parse
	if (!response || !*response) {
		showError(Request, " - no api response");
		return FALSE;
	}

	// Validate the response's error code
	if(!strstr(response, "code") || !parse(response)) {
		showError(Request, response);
		return FALSE;
	} else {
		const char* code = parse(response, "code");
		if(0 != strcmp(code, "0")){
			showError(Request, response);
			return FALSE;
		}
		// else 
		//	response is genuine, continue
	}

	// Validate the response is as expected
	if (names && *names && values && *values && nameValuePairCount > 0) {
		//int i;
		for (int i = 0; i < nameValuePairCount; i++) {
			const char* name = names[i];
			const char* value = values[i];
			if (!strstr(response, name) || !strstr(response, value)) {
				showError(Request, response);
				return FALSE;
			}
		}
	}

	return TRUE;
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

	char Path[512] = "/api/v5/public/time";
	char* Response = send(Path, NULL, 1);
	if (!isResponseOk(Path, NULL, Response, NULL, NULL, 0)) return 0;
	if (pTimeGMT) *pTimeGMT = convertTime(_atoi64(parse(Response, "ts")));

	return 2;
}


DLLFUNC int BrokerAccount(char* Account,double *pdBalance,double *pdTradeVal,double *pdMarginVal)
{
	if(!isConnected(1)) return 0;

	if(!Account || !*Account) 
		Account = (char *)"BTC";

	char Path[512] = "/api/v5/account/balance";
	char* Response = send(Path, NULL, 1);
	if (!isResponseOk(Path, NULL, Response, NULL, NULL, 0)) return 0;

	// Find '"details": [{' where '"ccy": "BTC",' or '"ccy": "<Account>",'
	// then
	// pBalance = "eqUsd"
	// pTradeVal = "eqUsd" * "notionalLever" ?!?
	// pMarginVal = "frozenBal" * "eqUsd" / "eq"
	double Balance = 0;
	double TradeVal = 0;
	double MarginVal = 0;
	while (1) {
		const char* Found = parse(NULL, "ccy");
		if (!Found || !*Found) break;
		if (0 == strcmp(Found, Account)){
			double eqUsd = atof(parse(NULL, "eqUsd"));
			Balance += eqUsd;
			
			double notionalLever = atof(parse(NULL, "notionalLever"));
			TradeVal += eqUsd * notionalLever;
			
			double eq = atof(parse(NULL, "eq"));
			double frozenBal = atof(parse(NULL, "frozenBal"));
			if(eq > 0.) MarginVal += frozenBal * eqUsd / eq;
		}
	}
	if (pdBalance) *pdBalance = Balance;
	if (pdTradeVal) *pdTradeVal = TradeVal;
	if (pdMarginVal) *pdMarginVal = MarginVal;
	return Balance > 0. ? 1 : 0;
}


DLLFUNC int BrokerAsset(char* Asset,double* pPrice,double* pSpread,
	double *pVolume, double *pPip, double *pPipCost, double *pMinAmount,
	double *pMarginCost, double *pRollLong, double *pRollShort)
{
	if(!isConnected()) return 0;

	BOOL isIndex = isIndexAsset(Asset);

	char Path[512] = "/api/v5/market/ticker?instId=";
	if (isIndex) 
		strcpy_s(Path, "/api/v5/market/index-tickers?instId=");
	strcat_s(Path, Asset);
	char* Response =	send(Path,NULL,0);
	if (!isResponseOk(Path, NULL, Response, NULL, NULL, 0)) return 0;

	if (isIndex) {
		double Bid = atof(parse(Response, "idxPx"));
		double Ask = atof(parse(Response, "idxPx"));
		if (Ask == 0. || Bid == 0.) return 0;
		if (pPrice) *pPrice = Ask;
		if (pSpread) *pSpread = Ask - Bid;
	}else{
		double Bid = atof(parse(Response,"bidPx"));
		double Ask = atof(parse(Response,"askPx"));
		double Volume = atof(parse(Response,"bidSz"))+atof(parse(Response,"askSz"));
		if(Ask == 0. || Bid == 0.) return 0;
		if(pPrice) *pPrice = Ask;
		if(pSpread) *pSpread = Ask - Bid;
		if(pVolume) *pVolume = Volume;
	}
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

	nTicks = min(nTicks, 100);

	BOOL isIndex = isIndexAsset(Asset);
	char Path[512] = "/api/v5/market/history-candles?instId=";
	if (isIndex)
		strcpy_s(Path, "/api/v5/market/index-candles?instId=");
	strcat_s(Path, Asset);
	strcat_s(Path, "&before=");
	strcat_s(Path, i64toa(convertTime(tStart)));
	strcat_s(Path, "&after=");
	strcat_s(Path, i64toa(convertTime(tEnd)));
	strcat_s(Path, "&bar=");
	strcat_s(Path, tf);
	strcat_s(Path, "&limit=");
	strcat_s(Path, itoa(nTicks));
	char* Response = send(Path, NULL, 0);
	char* dup = _strdup(Response); // need to duplicate the response as we will not use parse after
	if (!isResponseOk(Path, NULL, Response, NULL, NULL, 0)) return 0;

	char *Found = strchr(dup, '[');
	if (Found && *Found) {
		int i = 0;
		for (; i < nTicks; i++, ticks++) {
			Found = strchr(++Found, '[');
			if (!Found || !*Found) break;
			__int64 TimeClose;
			sscanf(Found, "[\"%I64d\",\"%f\",\"%f\",\"%f\",\"%f\",\"%f\"]",
				&TimeClose, &ticks->fOpen, &ticks->fHigh, &ticks->fLow, &ticks->fClose, &ticks->fVol);
			ticks->time = convertTime(TimeClose);
		}
		free(dup);
		return i;
	}
	free(dup);

	if(g_Warned++ <= 1) showError(Asset,"no data");
	return 0;
}



// returns negative amount when the trade was closed
DLLFUNC int BrokerTrade(int nTradeID,double *pOpen,double *pClose,double *pCost,double *pProfit)
{
	if(!isConnected(1)) return 0;

	char Path[512] = "/api/v5/trade/order?clOrdId=";
	const char* clOrdId = itoa(nTradeID);
	strcat_s(Path, clOrdId);
	strcat_s(Path, "&instId=");
	strcat_s(Path, g_Asset);
	char* Response = send(Path, NULL, 1);
	const char *names[] = { "clOrdId", "instId" };
	const char *values[] = { clOrdId, g_Asset };
	if (!isResponseOk(Path, NULL, Response, names, values, 2)) return 0;

/*// response
{
  "code": "0",
  "msg": "",
  "data": [
	{
	  "instType": "FUTURES",
	  "instId": "BTC-USD-200329",
	  "ccy": "",
	  "ordId": "312269865356374016",
	  "clOrdId": "b1",
	  "px": "999",
	  "sz": "3",
	  "pnl": "5",
	  "ordType": "limit",
	  "side": "buy",
	  "posSide": "long",
	  "tdMode": "isolated",
	  "accFillSz": "0",
	  "fillPx": "0",
	  "tradeId": "0",
	  "fillSz": "0",
	  "fillTime": "0",
	  "state": "live",
	  "avgPx": "0",
	  "lever": "20",
	  "feeCcy": "",
	  "fee": "",
	  "uTime": "1597026383085",
	  "cTime": "1597026383085"
	}
  ]
}
*/
	double Price = atof(parse(Response, "avgPx"));
	if (pOpen) *pOpen = Price;
	double Cost = atof(parse(Response, "fee"));
	if (pCost) *pCost = Cost;
	double Profit = atof(parse(Response, "pnl"));
	if (pProfit) *pProfit = Profit;
	int Fill = atof(parse(Response,"accFillSz"))/g_Amount;
	return Fill;
}

DLLFUNC int BrokerBuy2(char* Asset,int Amount,double dStopDist,double Limit,double *pPrice,int *pFill)
{
	if(!isConnected(1)) return 0;
	strcpy_s(g_Asset,Asset); // for BrokerTrade

	char Path[512] = "/api/v5/trade/order";
	char side[16];
	if (Amount > 0)
		strcat_s(side, "BUY");
	else
		strcat_s(side, "SELL");
	char ordType[16], px[6];
	if (Limit > 0. && (g_OrderType == 1 || g_OrderType == 5)) {
		double TickSize = 0.01; // todo: fix this
		Limit = ((int)(Limit / TickSize))*TickSize;	// clip to integer multiple of tick size
		if (g_OrderType == 1)
			strcat_s(ordType, "limit");
		else
			strcat_s(ordType, "optimal_limit_ioc");
		strcat_s(px, ftoa(Limit));
	} 
	//else if (g_OrderType == 1)
	//	strcat_s(ordType, "limit");
	else if (g_OrderType == 2)
		strcat_s(ordType, "post_only");
	else if (g_OrderType == 3)
		strcat_s(ordType, "fok");
	else if (g_OrderType == 4)
		strcat_s(ordType, "ioc");
	//else if (g_OrderType == 5)
	//	strcat_s(ordType, "optimal_limit_ioc");
	else //if (g_OrderType == 0)
		strcat_s(ordType, "market");
	char sz[6];
	strcat_s(sz, ftoa(g_Amount*labs(Amount)));
	char Body[512];
	int iClOrdId = g_Id++;
	char* clOrdId = itoa(iClOrdId);
	sprintf_s(Body, "{ \"instId\":%s, \"clOrdId\": %s, \"tdMode\": %s, \"side\": %s, \"ordType\": %s, \"px\": %s, \"sz\": %s}", g_Asset, clOrdId, g_TradeMode, side, ordType, px, sz);
	char* Response = send(Path, Body, 1);
	const char *names[] = { "clOrdId" };
	const char *values[] = { clOrdId };
	if (!isResponseOk(Path, Body, Response, names, values, 1)) return 0;
/*// response
{
  "code": "0",
  "msg": "",
  "data": [
	{
	  "clOrdId": "oktswap6",
	  "ordId": "312269865356374016",
	  "tag": "",
	  "sCode": "0",
	  "sMsg": ""
	}
  ]
}
*/

	//char Path2[512] = "/api/v5/trade/order";
	strcat_s(Path, "?clOrdId=");
	strcat_s(Path, clOrdId);
	strcat_s(Path, "&instId=");
	strcat_s(Path, g_Asset);
	Response = send(Path, NULL, 1);
	if (!isResponseOk(Path, NULL, Response, names, values, 1)) return 0;

	int Fill = atof(parse(Response,"accFillSz"))/g_Amount;
	double Price = atof(parse(NULL,"avgPx"));
	if(pPrice && Price > 0.) *pPrice = Price;
	if(pFill) *pFill = Fill;
	if(g_OrderType == 2 || Fill == Amount)		// TODO: check the NFA, Hedging and Order Type settings
		return iClOrdId; // fully filled or GTC
	if(Fill)
		return iClOrdId; // IOC partially or FOK fully filled
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
			char* Asset = (char*)parameter;
			if (!Asset || !*Asset) return 0;
			strcpy_s(g_Asset, Asset);
			return 1;
		}

		case SET_POSITION_MODE: {
			char* PositionMode = (char*)parameter;
			if (!PositionMode || !*PositionMode) return 0;
			char Path[64] = "/api/v5/account/set-position-mode";
			char Body[64];
			sprintf_s(Body, "{\"posMode\":\"%s\"}", PositionMode);
			char* Response = send(Path, Body, 1);
			const char *names[] = { "posMode" };
			const char *values[] = { PositionMode };
			if (!isResponseOk(Path, Body, Response, names, values, 1)) return 0;
			strcpy_s(g_PositionMode, PositionMode);
			return 1;
		}

		case SET_ACCOUNT_LEVERAGE: {
			var* parameters = (var*)parameter;
			if (!isConnected(1)) return 0;
			char Path[512] = "/api/v5/account/set-leverage";
			char Body[512];
			char* instId = (char*)parameters;// [0];
			if (!instId || !*instId) return 0;
			char* lever = (char*)parameters;// [1];
			if (!lever || !*lever) return 0;
			char* mgnMode = (char*)parameters;// [2];
			if (!mgnMode || !*mgnMode) return 0;
			char* posSide = (char*)parameters;// [3];
			if (!posSide || !*posSide) return 0;
			sprintf_s(Body, "{\"instId\": \"%s\",\"lever\": \"%s,\"mgnMode\": \"%s,\"posSide\": \"%s\"}", instId, lever, mgnMode, posSide);
			char* Response = send(Path, Body, 1);
			const char *names[] = { "instId", "lever", "mgnMode", "posSide" };
			const char *values[] = { instId, lever, mgnMode, posSide };
			if (!isResponseOk(Path, Body, Response, names, values, 4)) return 0;
			return 1;
		}

		case SET_TRADE_MODE: {
			char* TradeMode = (char*)parameter;
			if (!TradeMode || !*TradeMode) return 0;
			strcpy_s(g_TradeMode, TradeMode);
			return 1;
		}

		case GET_POSITION: {
			double Balance = 0;
			char* Asset = (char*)parameter;
			int Len = (int)strlen(Asset);
			if(Len > 5) Asset[Len-3] = 0; // clip trailing "BTC"
			BrokerAccount(Asset,&Balance,NULL,NULL);
			return Balance;
		}

		case DO_CANCEL: {
			if(!isConnected(1)) return 0;
			char Path[512] = "/api/v5/trade/cancel-order";
			char Body[512];
			sprintf_s(Body, "{\"clOrdId\": \"%s\",\"instId\": \"%s\"}", itoa(parameter), g_Asset);
			char* Response = send(Path, Body, 1);
			const char *names[] = { "clOrdId" };
			const char *values[] = { (char*)parameter };
			if (!isResponseOk(Path, Body, Response, names, values, 1)) return 0;
			return 1;
		}

		case GET_BOOK: {
/*
			T2* Quotes = (T2*)parameter;
			if(!Quotes) return 0;
			char* Response =	send("public/getmarketsummary?market=",g_Asset,1);
			if(Response && *Response)
				Quotes[0].time = convertTime(parse(Response,"TimeStamp"));
			else
				return 0;
			int N = 0;
			Response = send("public/getorderbook?type=sell&market=",g_Asset,0);
			if(!Response) return N;
			char* Success = parse(Response,"success");
			if(!strstr(Success,"true")) return N;
			for(; N<MAX_QUOTES/2; N++) {
				Quotes[N].fVol = atof(parse(NULL,"Quantity"));
				Quotes[N].fVal = atof(parse(NULL,"Rate"));	// ask
				if(Quotes[N].fVal == 0.) 
					break;
				Quotes[N].time = Quotes[0].time;
			}
			Response = send("public/getorderbook?type=buy&market=",g_Asset,0);
			if(!Response) return N;
			Success = parse(Response,"success");
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
