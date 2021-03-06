// Copyright (c) 2010-2011 Vincent Durham
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
//
#include "headers.h"

#include "namecoin.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"

using namespace json_spirit;

static const bool NAME_DEBUG = true;
typedef Value(*rpcfn_type)(const Array& params, bool fHelp);
extern map<string, rpcfn_type> mapCallTable;
extern int64 AmountFromValue(const Value& value);
extern Object JSONRPCError(int code, const string& message);
template<typename T> void ConvertTo(Value& value);

extern bool SelectCoins(int64 nTargetValue, set<CWalletTx*>& setCoinsRet);

static const int NAMECOIN_TX_VERSION = 0x7100;
static const int64 MIN_AMOUNT = CENT;
static const int MAX_NAME_LENGTH = 255;
static const int MAX_VALUE_LENGTH = 1023;
static const int OP_NAME_INVALID = 0x00;
static const int OP_NAME_NEW = 0x01;
static const int OP_NAME_FIRSTUPDATE = 0x02;
static const int OP_NAME_UPDATE = 0x03;
static const int OP_NAME_NOP = 0x04;
static const int MIN_FIRSTUPDATE_DEPTH = 12;
static const int EXPIRATION_DEPTH = 12000;

map<vector<unsigned char>, uint256> mapMyNames;
extern CCriticalSection cs_mapWallet;

// forward decls
extern bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch);
extern bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc);
extern int IndexOfNameOutput(CWalletTx& wtx);
extern bool Solver(const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CTransaction& txTo, unsigned int nIn, int nHashType);
extern bool GetValueOfNameTx(const CTransaction& tx, vector<unsigned char>& value);

const int NAME_COIN_GENESIS_EXTRA = 521;
uint256 hashNameCoinGenesisBlock("000000000062b72c5e2ceb45fbc8587e807c155b0da735e6483dfba2f0a9c770");

class CNamecoinHooks : public CHooks
{
public:
    virtual bool IsStandard(const CScript& scriptPubKey);
    virtual void AddToWallet(CWalletTx& tx);
    virtual bool CheckTransaction(const CTransaction& tx);
    virtual bool ConnectInputs(CTxDB& txdb,
            const CTransaction& tx,
            vector<CTransaction>& vTxPrev,
            vector<CTxIndex>& vTxindex,
            CBlockIndex* pindexBlock,
            CDiskTxPos& txPos,
            bool fBlock,
            bool fMiner);
    virtual bool DisconnectInputs(CTxDB& txdb,
            const CTransaction& tx,
            CBlockIndex* pindexBlock);
    virtual bool ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool ExtractAddress(const CScript& script, string& address);
    virtual bool GenesisBlock(CBlock& block);
    virtual bool Lockin(int nHeight, uint256 hash);
    virtual int LockinHeight();
    virtual string IrcPrefix();

    virtual void MessageStart(char* pchMessageStart)
    {
        // Make the message start different
        pchMessageStart[3] = 0xfe;
    }
};

int64 getAmount(Value value)
{
    ConvertTo<double>(value);
    double dAmount = value.get_real();
    int64 nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(-3, "Invalid amount");
    return nAmount;
}

vector<unsigned char> vchFromValue(const Value& value) {
    string strName = value.get_str();
    unsigned char *strbeg = (unsigned char*)strName.c_str();
    return vector<unsigned char>(strbeg, strbeg + strName.size());
}

vector<unsigned char> vchFromString(string str) {
    unsigned char *strbeg = (unsigned char*)str.c_str();
    return vector<unsigned char>(strbeg, strbeg + str.size());
}

string stringFromVch(vector<unsigned char> vch) {
    string res;
    vector<unsigned char>::iterator vi = vch.begin();
    while (vi != vch.end()) {
        res += (char)(*vi);
        vi++;
    }
    return res;
}

int64 GetNetworkFee(int nHeight)
{
  int64 nStart = 50 * COIN;
  if (fTestNet)
      nStart = 10 * CENT;
  int64 nRes = nStart >> (nHeight >> 13);
  nRes -= (nRes >> 14) * (nHeight % 8192);
  return nRes;
}

int GetTxPosHeight(const CDiskTxPos& txPos)
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txPos.nFile, txPos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return pindex->nHeight;
}


int GetNameHeight(CTxDB& txdb, vector<unsigned char> vchName) {
    CNameDB dbName("cr", txdb);
    vector<CDiskTxPos> vtxPos;
    if (dbName.ExistsName(vchName))
    {
        if (!dbName.ReadName(vchName, vtxPos))
            return error("GetNameHeight() : failed to read from name DB");
        if (vtxPos.empty())
            return -1;
        CDiskTxPos& txPos = vtxPos.back();
        return GetTxPosHeight(txPos);
    }
    return -1;
}

CScript RemoveNameScriptPrefix(const CScript& scriptIn)
{
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeNameScript(scriptIn, op, vvch,  pc))
        throw runtime_error("RemoveNameScriptPrefix() : could not decode name script");
    return CScript(pc, scriptIn.end());
}

bool SignNameSignature(const CTransaction& txFrom, CTransaction& txTo, unsigned int nIn, int nHashType=SIGHASH_ALL, CScript scriptPrereq=CScript())
{
    assert(nIn < txTo.vin.size());
    CTxIn& txin = txTo.vin[nIn];
    assert(txin.prevout.n < txFrom.vout.size());
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    // Leave out the signature from the hash, since a signature can't sign itself.
    // The checksig op will also drop the signatures from its hash.

    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    uint256 hash = SignatureHash(scriptPrereq + txout.scriptPubKey, txTo, nIn, nHashType);

    if (!Solver(scriptPubKey, hash, nHashType, txin.scriptSig))
        return false;

    txin.scriptSig = scriptPrereq + txin.scriptSig;

    // Test solution
    if (scriptPrereq.empty())
        if (!VerifyScript(txin.scriptSig, txout.scriptPubKey, txTo, nIn, 0))
            return false;

    return true;
}


bool CreateTransactionWithInputTx(const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxIn, int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    int64 nValue = 0;
    foreach (const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    CRITICAL_BLOCK(cs_main)
    {
        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        CRITICAL_BLOCK(cs_mapWallet)
        {
            nFeeRet = nTransactionFee;
            loop
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                printf("total value = %d\n", nTotalValue);
                double dPriority = 0;
                // vouts to the payees
                foreach (const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                int64 nWtxinCredit;

                if (wtxIn.fSpent)
                {
                    // non-name outputs have been spent, only grab name output value
                    nWtxinCredit = wtxIn.vout[nTxOut].nValue;
                    printf("input credit / spent = %d\n", nWtxinCredit);
                }
                else
                {
                    // no part of wtxIn was spent, grab the entire coin
                    nWtxinCredit = wtxIn.GetCredit();
                    printf("input credit / non-spent = %d\n", nWtxinCredit);
                }

                // Choose coins to use
                set<CWalletTx*> setCoins;
                if (!SelectCoins(nTotalValue - nWtxinCredit, setCoins))
                    return false;
                int64 nValueIn = 0;

                vector<CWalletTx*> vecCoins(setCoins.begin(), setCoins.end());

                foreach(CWalletTx* pcoin, vecCoins)
                {
                    // wtxIn non-name outputs might have been spent
                    int64 nCredit = pcoin->GetCredit();
                    nValueIn += nCredit;
                    dPriority += (double)nCredit * pcoin->GetDepthInMainChain();
                }

                // Input tx always at first position
                vecCoins.insert(vecCoins.begin(), &wtxIn);

                nValueIn += nWtxinCredit;
                dPriority += (double)nWtxinCredit * wtxIn.GetDepthInMainChain();

                // Fill a vout back to self with any change
                int64 nChange = nValueIn - nTotalValue;
                printf("change = %d\n", nChange);
                if (nChange >= CENT)
                {
                    // Note: We use a new key here to keep it from being obvious which side is the change.
                    //  The drawback is that by not reusing a previous key, the change may be lost if a
                    //  backup is restored, if the backup doesn't have the new private key for the change.
                    //  If we reused the old key, it would be possible to add code to look for and
                    //  rediscover unknown transactions that were written with keys of ours to recover
                    //  post-backup change.

                    // Reserve a new key pair from key pool
                    vector<unsigned char> vchPubKey = reservekey.GetReservedKey();
                    assert(mapKeys.count(vchPubKey));

                    // Fill a vout to ourself, using same address type as the payment
                    CScript scriptChange;
                    if (vecSend[0].first.GetBitcoinAddressHash160() != 0)
                        scriptChange.SetBitcoinAddress(vchPubKey);
                    else
                        scriptChange << vchPubKey << OP_CHECKSIG;

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                foreach(CWalletTx* pcoin, vecCoins)
                    for (int nOut = 0; nOut < pcoin->vout.size(); nOut++)
                    {
                        // three cases:
                        // * this is wtxIn name output, which can only be spent by this function - grab it
                        // * this is a wtxIn non-name output and the non-name part of the coin was already spent - skip
                        // * this is not wtxIn - we already checked it wasn't spent, grab it
                        if (pcoin == &wtxIn && nOut == nTxOut)
                        {
                            if (pcoin->vout[nOut].IsMine())
                                throw runtime_error("CreateTransactionWithInputTx() : wtxIn[nTxOut] already mine");
                            wtxNew.vin.push_back(CTxIn(pcoin->GetHash(), nOut));
                        }
                        else if (!pcoin->fSpent && pcoin->vout[nOut].IsMine())
                            wtxNew.vin.push_back(CTxIn(pcoin->GetHash(), nOut));
                    }

                // Sign
                int nIn = 0;
                foreach(CWalletTx* pcoin, vecCoins)
                    for (int nOut = 0; nOut < pcoin->vout.size(); nOut++)
                    {
                        if (pcoin == &wtxIn && nOut == nTxOut)
                        {
                            if (!SignNameSignature(*pcoin, wtxNew, nIn++))
                                throw runtime_error("could not sign name coin output");
                        }
                        else if (!pcoin->fSpent && pcoin->vout[nOut].IsMine())
                            if (!SignSignature(*pcoin, wtxNew, nIn++))
                                return false;
                    }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                bool fAllowFree = CTransaction::AllowFree(dPriority);
                int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

// nTxOut is the output from wtxIn that we should grab
string SendMoneyWithInputTx(CScript scriptPubKey, int64 nValue, int64 nNetFee, CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee)
{
    int nTxOut = IndexOfNameOutput(wtxIn);
    CRITICAL_BLOCK(cs_main)
    {
        CReserveKey reservekey;
        int64 nFeeRequired;
        vector< pair<CScript, int64> > vecSend;
        vecSend.push_back(make_pair(scriptPubKey, nValue));

        if (nNetFee)
        {
            CScript scriptFee;
            scriptFee << OP_RETURN;
            vecSend.push_back(make_pair(scriptFee, nNetFee));
        }

        if (!CreateTransactionWithInputTx(vecSend, wtxIn, nTxOut, wtxNew, reservekey, nFeeRequired))
        {
            string strError;
            if (nValue + nFeeRequired > GetBalance())
                strError = strprintf(_("Error: This is an oversized transaction that requires a transaction fee of %s  "), FormatMoney(nFeeRequired).c_str());
            else
                strError = _("Error: Transaction creation failed  ");
            printf("SendMoney() : %s", strError.c_str());
            return strError;
        }

        if (fAskFee && !ThreadSafeAskFee(nFeeRequired, _("Sending..."), NULL))
            return "ABORTED";

        if (!CommitTransaction(wtxNew, reservekey))
            return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");
    }
    MainFrameRepaint();
    return "";
}


bool GetValueOfTxPos(const CDiskTxPos& txPos, vector<unsigned char>& vchValue, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    if (!GetValueOfNameTx(tx, vchValue))
        return error("GetValueOfTxPos() : could not decode value from tx");
}

bool GetValueOfName(CNameDB& dbName, vector<unsigned char> vchName, vector<unsigned char>& vchValue, int& nHeight)
{
    vector<CDiskTxPos> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    CDiskTxPos& txPos = vtxPos.back();
    return GetValueOfTxPos(txPos, vchValue, nHeight);
}

Value name_list(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
                "name_list [<name>]\n"
                "list my own names"
                );

    map<vector<unsigned char>, uint256>::iterator mi;
    if (params.size() > 0)
    {
        vector<unsigned char> vchName = vchFromValue(params[0]);
        mi = mapMyNames.find(vchName);
    }
    else
    {
        mi = mapMyNames.begin();
    }

    CNameDB dbName("cr");
    Array oRes;

    while (mi != mapMyNames.end()) {
        Object oName;
        string name = stringFromVch((*mi).first);
        oName.push_back(Pair("name", name));
        vector<unsigned char> vchValue;
        int nHeight;
        if (GetValueOfName(dbName, (*mi).first, vchValue, nHeight))
        {
            string value = stringFromVch(vchValue);
            oName.push_back(Pair("value", value));
            oName.push_back(Pair("expires_in", nHeight + EXPIRATION_DEPTH - pindexBest->nHeight));
        }
        else
        {
            oName.push_back(Pair("expired", 1));
        }
        oRes.push_back(oName);
        mi++;
    }

    if (NAME_DEBUG) {
        dbName.test();
    }
    return oRes;
}

Value name_scan(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
                "name_scan [<start-name>] [<max-returned>]\n"
                "scan all names, starting at start-name and returning a maximum number of entries (default 500)\n"
                );

    vector<unsigned char> vchName;
    int nMax = 500;
    if (params.size() > 0)
    {
        vchName = vchFromValue(params[0]);
    }

    if (params.size() > 1)
    {
        Value vMax = params[1];
        ConvertTo<double>(vMax);
        nMax = (int)vMax.get_real();
    }

    CNameDB dbName("r");
    Array oRes;

    vector<pair<vector<unsigned char>, CDiskTxPos> > nameScan;
    if (!dbName.ScanNames(vchName, nMax, nameScan))
        throw JSONRPCError(-4, "scan failed");

    pair<vector<unsigned char>, CDiskTxPos> pairScan;
    foreach (pairScan, nameScan)
    {
        Object oName;
        string name = stringFromVch(pairScan.first);
        CDiskTxPos txPos = pairScan.second;
        oName.push_back(Pair("name", name));
        vector<unsigned char> vchValue;
        int nHeight;
        if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, nHeight))
        {
            string value = stringFromVch(vchValue);
            oName.push_back(Pair("value", value));
            oName.push_back(Pair("expires_in", nHeight + EXPIRATION_DEPTH - pindexBest->nHeight));
        }
        else
        {
            oName.push_back(Pair("expired", 1));
        }
        oRes.push_back(oName);
    }

    if (NAME_DEBUG) {
        dbName.test();
    }
    return oRes;
}

Value name_firstupdate(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
                "name_firstupdate <name> <rand> [<tx>] <value> <network-fee-amount>\n"
                "Perform a first update after a name_new reservation.\n"
                "Note that the first update will go into a block 12 blocks after the name_new, at the soonest."
                );
    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchRand = ParseHex(params[1].get_str());
    vector<unsigned char> vchTx;
    vector<unsigned char> vchValue;

    if (params.size() == 3)
    {
        vchValue = vchFromValue(params[2]);
    }
    else
    {
        vchValue = vchFromValue(params[3]);
    }


    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;
    vector<unsigned char> strPubKey = GetKeyFromKeyPool();
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetBitcoinAddress(strPubKey);
    CScript scriptPubKey;
    scriptPubKey << OP_NAME_FIRSTUPDATE << vchName << vchRand << vchValue << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

    CRITICAL_BLOCK(cs_main)
    {
        // Make sure there is a previous NAME_NEW tx on this name
        // and that the random value matches
        uint256 wtxInHash;
        if (params.size() == 3)
        {
            if (!mapMyNames.count(vchName))
            {
                throw runtime_error("could not find a coin with this name");
            }
            wtxInHash = mapMyNames[vchName];
        }
        else
        {
            wtxInHash.SetHex(params[2].get_str());
        }
        CWalletTx& wtxIn = mapWallet[wtxInHash];
        vector<unsigned char> vchHash;
        bool found = false;
        foreach (CTxOut& out, wtxIn.vout)
        {
            vector<vector<unsigned char> > vvch;
            int op;
            if (DecodeNameScript(out.scriptPubKey, op, vvch)) {
                if (op != OP_NAME_NEW)
                    throw runtime_error("previous transaction wasn't a name_new");
                vchHash = vvch[0];
                found = true;
            }
        }

        if (!found)
        {
            throw runtime_error("previous tx on this name is not a name tx");
        }
        vector<unsigned char> vchToHash(vchRand);
        vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
        uint160 hash =  Hash160(vchToHash);
        if (uint160(vchHash) != hash)
        {
            throw runtime_error("previous tx used a different random value");
        }

        int64 nNetFee = GetNetworkFee(pindexBest->nHeight);
        // Round up to CENT
        nNetFee += CENT - 1;
        nNetFee = (nNetFee / CENT) * CENT;
        string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, nNetFee, wtxIn, wtx, false);
        if (strError != "")
            throw JSONRPCError(-4, strError);
    }
    return wtx.GetHash().GetHex();
}

Value name_update(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
                "name_update <name> <value> <network-fee-amount>\n"
                );

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;
    vector<unsigned char> strPubKey = GetKeyFromKeyPool();
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetBitcoinAddress(strPubKey);
    CScript scriptPubKey;
    scriptPubKey << OP_NAME_UPDATE << vchName << vchValue << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    CRITICAL_BLOCK(cs_main)
    {
        if (!mapMyNames.count(vchName))
        {
            throw runtime_error("could not find a coin with this name");
        }
        uint256 wtxInHash = mapMyNames[vchName];
        CWalletTx& wtxIn = mapWallet[wtxInHash];
        string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, 0, wtxIn, wtx, false);
        if (strError != "")
            throw JSONRPCError(-4, strError);
    }
    return wtx.GetHash().GetHex();
}

Value name_new(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
                "name_new <name>\n"
                );

    vector<unsigned char> vchName = vchFromValue(params[0]);

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    uint64 rand = GetRand((uint64)-1);
    vector<unsigned char> vchRand = CBigNum(rand).getvch();
    vector<unsigned char> vchToHash(vchRand);
    vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
    uint160 hash =  Hash160(vchToHash);

    vector<unsigned char> strPubKey = GetKeyFromKeyPool();
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetBitcoinAddress(strPubKey);
    CScript scriptPubKey;
    scriptPubKey << OP_NAME_NEW << hash << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

    string strError = SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);
    if (strError != "")
        throw JSONRPCError(-4, strError);
    mapMyNames[vchName] = wtx.GetHash();
    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(HexStr(vchRand));
    return res;
}

bool CNameDB::test()
{
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    loop
    {
        // Read next record
        CDataStream ssKey;
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue);
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            string strName = stringFromVch(vchName);
            vector<CDiskTxPos> vtxPos;
            ssValue >> vtxPos;
            if (NAME_DEBUG)
              printf("NAME %s : ", strName.c_str());
            foreach(CDiskTxPos& txPos, vtxPos) {
                txPos.print();
                if (NAME_DEBUG)
                  printf(" @ %d, ", GetTxPosHeight(txPos));
            }
            if (NAME_DEBUG)
              printf("\n");
        }
    }
    pcursor->close();
}

bool CNameDB::ScanNames(
        const vector<unsigned char>& vchName,
        int nMax,
        vector<pair<vector<unsigned char>, CDiskTxPos> >& nameScan)
{
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey;
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("namei"), vchName);
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            string strName = stringFromVch(vchName);
            vector<CDiskTxPos> vtxPos;
            ssValue >> vtxPos;
            CDiskTxPos txPos;
            if (!vtxPos.empty())
            {
                txPos = vtxPos.back();
            }
            nameScan.push_back(make_pair(vchName, txPos));
        }

        if (nameScan.size() >= nMax)
            break;
    }
    pcursor->close();
    return true;
}

CHooks* InitHook()
{
    mapCallTable.insert(make_pair("name_new", &name_new));
    mapCallTable.insert(make_pair("name_update", &name_update));
    mapCallTable.insert(make_pair("name_firstupdate", &name_firstupdate));
    mapCallTable.insert(make_pair("name_list", &name_list));
    mapCallTable.insert(make_pair("name_scan", &name_scan));
    hashGenesisBlock = hashNameCoinGenesisBlock;
    printf("Setup namecoin genesis block %s\n", hashGenesisBlock.GetHex().c_str());
    return new CNamecoinHooks();
}

bool CNamecoinHooks::IsStandard(const CScript& scriptPubKey)
{
    return true;
}

bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch)
{
    CScript::const_iterator pc = script.begin();
    return DecodeNameScript(script, op, vvch, pc);
}

bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc)
{
    opcodetype opcode;
    if (!script.GetOp(pc, opcode))
        return false;
    if (opcode < OP_1 || opcode > OP_16)
        return false;

    op = opcode - OP_1 + 1;

    for (;;) {
        vector<unsigned char> vch;
        if (!script.GetOp(pc, opcode, vch))
            return false;
        if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
            break;
        if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        vvch.push_back(vch);
    }

    // move the pc to after any DROP or NOP
    while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
    {
        if (!script.GetOp(pc, opcode))
            break;
    }

    pc--;

    if ((op == OP_NAME_NEW && vvch.size() == 1) ||
            (op == OP_NAME_FIRSTUPDATE && vvch.size() == 3) ||
            (op == OP_NAME_UPDATE && vvch.size() == 2))
        return true;
    return error("invalid number of arguments for name op");
}

bool DecodeNameTx(const CTransaction& tx, int& op, int& nOut, vector<vector<unsigned char> >& vvch)
{
    bool found = false;

    for (int i = 0 ; i < tx.vout.size() ; i++)
    {
        const CTxOut& out = tx.vout[i];
        if (DecodeNameScript(out.scriptPubKey, op, vvch))
        {
            // If more than one name op, fail
            if (found)
                return false;
            nOut = i;
            found = true;
        }
    }

    return found;
}

int64 GetNameNetFee(const CTransaction& tx)
{
    int64 nFee = 0;

    for (int i = 0 ; i < tx.vout.size() ; i++)
    {
        const CTxOut& out = tx.vout[i];
        if (out.scriptPubKey.size() == 1 && out.scriptPubKey[0] == OP_RETURN)
        {
            nFee += out.nValue;
        }
    }

    return nFee;
}

bool GetValueOfNameTx(const CTransaction& tx, vector<unsigned char>& value)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if (!DecodeNameTx(tx, op, nOut, vvch))
        return false;

    switch (op)
    {
        case OP_NAME_NEW:
            return false;
        case OP_NAME_FIRSTUPDATE:
            value = vvch[2];
            return true;
        case OP_NAME_UPDATE:
            value = vvch[1];
            return true;
        default:
            return false;
    }
}

int IndexOfNameOutput(CWalletTx& wtx)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(wtx, op, nOut, vvch);

    if (!good)
        throw runtime_error("IndexOfNameOutput() : name output not found");
    return nOut;
}

void CNamecoinHooks::AddToWallet(CWalletTx& wtx)
{
    if (wtx.nVersion != NAMECOIN_TX_VERSION)
        return;

    if (wtx.vout.size() < 1)
    {
        error("AddToWalletHook() : no output in name tx %s", wtx.ToString().c_str());
        return;
    }

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(wtx, op, nOut, vvch);

    if (!good)
    {
        error("AddToWalletHook() : no output out script in name tx %s", wtx.ToString().c_str());
        return;
    }

    CRITICAL_BLOCK(cs_mapWallet)
    {
        if (!wtx.fSpent && op != OP_NAME_NEW)
            mapMyNames[vvch[0]] = wtx.GetHash();
    }
}

int CheckTransactionAtRelativeDepth(CBlockIndex* pindexBlock, CTxIndex& txindex, int maxDepth)
{
    for (CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < maxDepth; pindex = pindex->pprev)
        if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
            return pindexBlock->nHeight - pindex->nHeight;
    return -1;
}

bool CNamecoinHooks::ConnectInputs(CTxDB& txdb,
        const CTransaction& tx,
        vector<CTransaction>& vTxPrev,
        vector<CTxIndex>& vTxindex,
        CBlockIndex* pindexBlock,
        CDiskTxPos& txPos,
        bool fBlock,
        bool fMiner)
{
    bool nInput;
    bool found = false;

    int prevOp;
    vector<vector<unsigned char> > vvchPrevArgs;

    for (int i = 0 ; i < tx.vin.size() ; i++) {
        CTxOut& out = vTxPrev[i].vout[tx.vin[i].prevout.n];
        if (DecodeNameScript(out.scriptPubKey, prevOp, vvchPrevArgs))
        {
            if (found)
                return error("ConnectInputHook() : multiple previous name transactions");
            found = true;
            nInput = i;
        }
    }

    if (tx.nVersion != NAMECOIN_TX_VERSION)
    {
        // Make sure name-op outputs are not spent by a regular transaction, or the name
        // would be lost
        if (found)
            return error("ConnectInputHook() : a non-namecoin transaction with a namecoin input");
        return true;
    }

    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("ConnectInputsHook() : could not decode a namecoin tx");

    int nPrevHeight;
    int nDepth;
    int64 nNetFee;

    switch (op)
    {
        case OP_NAME_NEW:
            if (found)
                return error("name_new tx pointing to previous namecoin tx");
            break;
        case OP_NAME_FIRSTUPDATE:
            nNetFee = GetNameNetFee(tx);
            if (nNetFee < GetNetworkFee(pindexBlock->nHeight))
                return error("got tx %s with fee too low %d", tx.GetHash().GetHex().c_str(), nNetFee);
            if (!found || prevOp != OP_NAME_NEW)
                return error("name_firstupdate tx without previous name_new tx");
            nPrevHeight = GetNameHeight(txdb, vvchArgs[0]);
            if (nPrevHeight >= 0 && pindexBlock->nHeight - nPrevHeight < EXPIRATION_DEPTH)
                return error("name_firstupdate on an unexpired name");
            nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], MIN_FIRSTUPDATE_DEPTH);
            // Do not accept if in chain and not mature
            if ((fBlock || fMiner) && nDepth >= 0 && nDepth < MIN_FIRSTUPDATE_DEPTH)
                return false;

            // Do not mine if previous name_new is not visible.  This is if
            // name_new expired or not yet in a block
            if (fMiner)
            {
                // TODO CPU intensive
                nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], EXPIRATION_DEPTH);
                if (nDepth == -1)
                    return error("name_firstupdate cannot be mined if name_new is not already in chain and unexpired");
            }
            break;
        case OP_NAME_UPDATE:
            if (!found || (prevOp != OP_NAME_FIRSTUPDATE && prevOp != OP_NAME_UPDATE))
                return error("name_update tx without previous update tx");
            // TODO this might be too CPU intensive, up to 12000 blocks to look through
            nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], EXPIRATION_DEPTH);
            if ((fBlock || fMiner) && nDepth < 0)
                return error("name_update on an expired name, or there is a pending transaction on the name");
            break;
        default:
            return error("name transaction has unknown op");
    }

    if (fBlock)
    {
        CNameDB dbName("cr+", txdb);

        dbName.TxnBegin();

        if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
        {
            vector<CDiskTxPos> vtxPos;
            if (dbName.ExistsName(vvchArgs[0]))
            {
                if (!dbName.ReadName(vvchArgs[0], vtxPos))
                    return error("ConnectBlockHook() : failed to read from name DB");
            }
            vtxPos.push_back(txPos);
            if (!dbName.WriteName(vvchArgs[0], vtxPos))
                return error("ConnectBlockHook() : failed to write to name DB");
        }

        dbName.TxnCommit();
    }

    return true;
}

bool CNamecoinHooks::DisconnectInputs(CTxDB& txdb,
        const CTransaction& tx,
        CBlockIndex* pindexBlock)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("ConnectBlockHook() : could not decode namecoin tx");
    if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
    {
        CNameDB dbName("cr+", txdb);

        dbName.TxnBegin();

        vector<CDiskTxPos> vtxPos;
        if (!dbName.ReadName(vvchArgs[0], vtxPos))
            return error("ConnectBlockHook() : failed to read from name DB");
        // vtxPos might be empty if we pruned expired transactions.  However, it should normally still not
        // be empty, since a reorg cannot go that far back.  Be safe anyway and do not try to pop if empty.
        if (vtxPos.size())
        {
            vtxPos.pop_back();
            // TODO validate that the first pos is the current tx pos
        }
        if (!dbName.WriteName(vvchArgs[0], vtxPos))
            return error("ConnectBlockHook() : failed to write to name DB");

        dbName.TxnCommit();
    }

    return true;
}

bool CNamecoinHooks::CheckTransaction(const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vector<unsigned char> > vvch;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        return error("name transaction has unknown script format");
    }

    if (vvch[0].size() > MAX_NAME_LENGTH)
    {
        return error("name transaction with name too long");
    }

    switch (op)
    {
        case OP_NAME_NEW:
            if (vvch[0].size() != 20)
            {
                return error("name_new tx with incorrect hash length");
            }
            break;
        case OP_NAME_FIRSTUPDATE:
            if (vvch[1].size() > 20)
            {
                return error("name_firstupdate tx with rand too big");
            }
            if (vvch[2].size() > MAX_VALUE_LENGTH)
            {
                return error("name_firstupdate tx with value too long");
            }
            break;
        case OP_NAME_UPDATE:
            if (vvch[1].size() > MAX_VALUE_LENGTH)
            {
                return error("name_update tx with value too long");
            }
            break;
        default:
            return error("name transaction has unknown op");
    }
    return true;
}

static string nameFromOp(int op)
{
    switch (op)
    {
        case OP_NAME_NEW:
            return "name_new";
        case OP_NAME_UPDATE:
            return "name_update";
        case OP_NAME_FIRSTUPDATE:
            return "name_firstupdate";
        default:
            return "<unknown name op>";
    }
}

bool CNamecoinHooks::ExtractAddress(const CScript& script, string& address)
{
    if (script.size() == 1 && script[0] == OP_RETURN)
    {
        address = string("network fee");
        return true;
    }
    vector<vector<unsigned char> > vvch;
    int op;
    if (!DecodeNameScript(script, op, vvch))
        return false;

    string strOp = nameFromOp(op);
    string strName = stringFromVch(vvch[0]);
    if (op == OP_NAME_NEW)
        strName = HexStr(vvch[0]);

    address = strOp + ": " + strName;
    return true;
}

bool CNamecoinHooks::ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

bool CNamecoinHooks::DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

bool GenesisBlock(CBlock& block, int extra)
{
    block = CBlock();
    block.hashPrevBlock = 0;
    block.nVersion = 1;
    block.nTime    = 1303000001;
    block.nBits    = 0x1c007fff;
    block.nNonce   = 0xa21ea192U;
    const char* pszTimestamp = "... choose what comes next.  Lives of your own, or a return to chains. -- V";
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << block.nBits << CBigNum(++extra) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = 50 * COIN;
    txNew.vout[0].scriptPubKey = CScript() << ParseHex("04b620369050cd899ffbbc4e8ee51e8c4534a855bb463439d63d235d4779685d8b6f4870a238cf365ac94fa13ef9a2a22cd99d0d5ee86dcabcafce36c7acf43ce5") << OP_CHECKSIG;
    block.vtx.push_back(txNew);
    block.hashMerkleRoot = block.BuildMerkleTree();
    printf("====================================\n");
    printf("Merkle: %s\n", block.hashMerkleRoot.GetHex().c_str());
    printf("Block: %s\n", block.GetHash().GetHex().c_str());
    block.print();
    assert(block.GetHash() == hashGenesisBlock);
    return true;
}

bool CNamecoinHooks::GenesisBlock(CBlock& block)
{
    if (fTestNet)
        return false;

    ::GenesisBlock(block, NAME_COIN_GENESIS_EXTRA);
}

int CNamecoinHooks::LockinHeight()
{
    return 0;
}

bool CNamecoinHooks::Lockin(int nHeight, uint256 hash)
{
    return true;
}

string CNamecoinHooks::IrcPrefix()
{
    return "namecoin";
}

unsigned short GetDefaultPort()
{
    return fTestNet ? htons(18334) : htons(8334);
}

unsigned int pnSeed[] = { NULL };
const char *strDNSSeed[] = { NULL };
