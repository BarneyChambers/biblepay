// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The D�sh Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodel.h"
#include "addresstablemodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "paymentserver.h"
#include "recentrequeststablemodel.h"
#include "transactiontablemodel.h"
#include "base58.h"
#include "wallet/crypter.h"
#include "keystore.h"
#include "main.h"
#include "sync.h"
#include "ui_interface.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h" // for BackupWallet
#include "darksend.h"
#include "instantx.h"
#include "spork.h"
#include <stdint.h>
#include <QDebug>
#include <QSet>
#include <QTimer>
#include <boost/foreach.hpp>


std::string PubKeyToAddress(const CScript& scriptPubKey);
extern QString ToQstring(std::string s);
extern std::string FromQStringW(QString qs);
std::string SubmitToIPFS(std::string sPath, std::string& sError);
double GetSporkDouble(std::string sName, double nDefault);
int64_t GetFileSize(std::string sPath);
std::string DefaultRecAddress(std::string sType);
// POG
TitheDifficultyParams GetTitheParams(const CBlockIndex* pindex);
// END OF POG


WalletModel::WalletModel(const PlatformStyle *platformStyle, CWallet *wallet, OptionsModel *optionsModel, QObject *parent) :
    QObject(parent), wallet(wallet), optionsModel(optionsModel), addressTableModel(0),
    transactionTableModel(0),
    recentRequestsTableModel(0),
    cachedBalance(0),
    cachedUnconfirmedBalance(0),
    cachedImmatureBalance(0),
    cachedAnonymizedBalance(0),
    cachedWatchOnlyBalance(0),
    cachedWatchUnconfBalance(0),
    cachedWatchImmatureBalance(0),
    cachedEncryptionStatus(Unencrypted),
    cachedNumBlocks(0),
    cachedTxLocks(0),
    cachedPrivateSendRounds(0)
{
    fHaveWatchOnly = wallet->HaveWatchOnly();
    fForceCheckBalanceChanged = false;

    addressTableModel = new AddressTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(platformStyle, wallet, this);
    recentRequestsTableModel = new RecentRequestsTableModel(wallet, this);

    // This timer will be fired repeatedly to update the balance
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(pollBalanceChanged()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

CAmount WalletModel::getBalance(const CCoinControl *coinControl) const
{
    if (coinControl)
    {
        CAmount nBalance = 0;
        std::vector<COutput> vCoins;
        wallet->AvailableCoins(vCoins, true, coinControl);
        BOOST_FOREACH(const COutput& out, vCoins)
            if(out.fSpendable)
                nBalance += out.tx->vout[out.i].nValue;

        return nBalance;
    }

    return wallet->GetBalance();
}


CAmount WalletModel::getAnonymizedBalance() const
{
    return wallet->GetAnonymizedBalance();
}

CAmount WalletModel::getUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedBalance();
}

CAmount WalletModel::getImmatureBalance() const
{
    return wallet->GetImmatureBalance();
}

bool WalletModel::haveWatchOnly() const
{
    return fHaveWatchOnly;
}

CAmount WalletModel::getWatchBalance() const
{
    return wallet->GetWatchOnlyBalance();
}

CAmount WalletModel::getWatchUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedWatchOnlyBalance();
}

CAmount WalletModel::getWatchImmatureBalance() const
{
    return wallet->GetImmatureWatchOnlyBalance();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus)
        Q_EMIT encryptionStatusChanged(newEncryptionStatus);
}



std::string FromQStringW(QString qs)
{
	std::string sOut = qs.toUtf8().constData();
	return sOut;
}



void WalletModel::pollBalanceChanged()
{
    // Get required locks upfront. This avoids the GUI from getting stuck on
    // periodical polls if the core is holding the locks for a longer time -
    // for example, during a wallet rescan.
    TRY_LOCK(cs_main, lockMain);
    if(!lockMain)
        return;
    TRY_LOCK(wallet->cs_wallet, lockWallet);
    if(!lockWallet)
        return;

    if(fForceCheckBalanceChanged || chainActive.Height() != cachedNumBlocks || nPrivateSendRounds != cachedPrivateSendRounds || cachedTxLocks != nCompleteTXLocks)
    {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        cachedNumBlocks = chainActive.Height();
        cachedPrivateSendRounds = nPrivateSendRounds;

        checkBalanceChanged();
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();
    }
}

void WalletModel::checkBalanceChanged()
{
    CAmount newBalance = getBalance();
    CAmount newUnconfirmedBalance = getUnconfirmedBalance();
    CAmount newImmatureBalance = getImmatureBalance();
    CAmount newAnonymizedBalance = getAnonymizedBalance();
    CAmount newWatchOnlyBalance = 0;
    CAmount newWatchUnconfBalance = 0;
    CAmount newWatchImmatureBalance = 0;
    if (haveWatchOnly())
    {
        newWatchOnlyBalance = getWatchBalance();
        newWatchUnconfBalance = getWatchUnconfirmedBalance();
        newWatchImmatureBalance = getWatchImmatureBalance();
    }

    if(cachedBalance != newBalance || cachedUnconfirmedBalance != newUnconfirmedBalance || cachedImmatureBalance != newImmatureBalance ||
        cachedAnonymizedBalance != newAnonymizedBalance || cachedTxLocks != nCompleteTXLocks ||
        cachedWatchOnlyBalance != newWatchOnlyBalance || cachedWatchUnconfBalance != newWatchUnconfBalance || cachedWatchImmatureBalance != newWatchImmatureBalance)
    {
        cachedBalance = newBalance;
        cachedUnconfirmedBalance = newUnconfirmedBalance;
        cachedImmatureBalance = newImmatureBalance;
        cachedAnonymizedBalance = newAnonymizedBalance;
        cachedTxLocks = nCompleteTXLocks;
        cachedWatchOnlyBalance = newWatchOnlyBalance;
        cachedWatchUnconfBalance = newWatchUnconfBalance;
        cachedWatchImmatureBalance = newWatchImmatureBalance;
        Q_EMIT balanceChanged(newBalance, newUnconfirmedBalance, newImmatureBalance, newAnonymizedBalance,
                            newWatchOnlyBalance, newWatchUnconfBalance, newWatchImmatureBalance);
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, const QString &purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString &address)
{
    CBitcoinAddress addressParsed(address.toStdString());
    return addressParsed.IsValid();
}


QString ToQstring(std::string s)
{
	QString str1 = QString::fromUtf8(s.c_str());
	return str1;
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl *coinControl)
{
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
	bool fForce = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    // This should never really happen, yet another safety check, just in case.
    if(wallet->IsLocked()) {
        return TransactionCreationFailed;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

	std::string sMessages = "";
	bool fTithed = false;

    // Pre-check input data for validity
    Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;
		if (rcp.fForce)
			fForce = true;

        if (rcp.paymentRequest.IsInitialized())
        {   // PaymentRequest...
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int i = 0; i < details.outputs_size(); i++)
            {
                const payments::Output& out = details.outputs(i);
                if (out.amount() <= 0) continue;
                subtotal += out.amount();
                const unsigned char* scriptStr = (const unsigned char*)out.script().data();
                CScript scriptPubKey(scriptStr, scriptStr+out.script().size());
                CAmount nAmount = out.amount();
                CRecipient recipient = {scriptPubKey, nAmount, rcp.fForce, rcp.fSubtractFeeFromAmount, rcp.fTithe, rcp.fPrayer, rcp.fRepent, 
					FromQStringW(rcp.txtMessage), FromQStringW(rcp.txtRepent), FromQStringW(""), FromQStringW(rcp.ipfshash) };
                vecSend.push_back(recipient);
            }
            if (subtotal <= 0)
            {
                return InvalidAmount;
            }
            total += subtotal;
        }
        else
        {   // User-entered biblepay address / amount:
            if(!validateAddress(rcp.address))
            {
                return InvalidAddress;
            }
            if(rcp.amount <= 0)
            {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(rcp.address.toStdString()).Get());
            CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fForce, rcp.fSubtractFeeFromAmount, rcp.fTithe, rcp.fPrayer, rcp.fRepent, 
				FromQStringW(rcp.txtMessage), FromQStringW(rcp.txtRepent), FromQStringW(""), FromQStringW(rcp.ipfshash) };
			//If TITHE is Checked, add a recipient here

			std::string sAddress = PubKeyToAddress(scriptPubKey);
			std::string sTitheAddress = DefaultRecAddress("TITHES");
			const Consensus::Params& consensusParams = Params().GetConsensus();
			
			if (sAddress == consensusParams.FoundationAddress)
			{
				// Donate to Foundation was checked
				sMessages += "<TITHER>" + sTitheAddress + "</TITHER><NICKNAME>" + msNickName + "</NICKNAME><TITHESIGNER>" + sTitheAddress + "</TITHESIGNER>";
				fTithed = true;
			}
			std::string sRepentNarr = "";
			LogPrintf(" \r\n Created Tx for Outbound Money %f to address %s  ",(double)rcp.amount,sAddress.c_str());

			vecSend.push_back(recipient);

			if (rcp.fTithe)
			{
				CAmount aTitheAmount = rcp.amount * .10;
				CScript spkFoundation = GetScriptForDestination(CBitcoinAddress(consensusParams.FoundationAddress).Get());
	            CRecipient recFoundation = {spkFoundation, aTitheAmount, rcp.fForce, false, true, rcp.fPrayer, rcp.fRepent, FromQStringW(rcp.txtMessage), 
					FromQStringW(rcp.txtRepent), FromQStringW(""), FromQStringW(rcp.ipfshash) };
				std::string sAddrF = PubKeyToAddress(spkFoundation);
				setAddress.insert(ToQstring(sAddrF));
           		++nAddresses;
				// POG - R ANDREWS - 11/20/2018
				LogPrintf(" \r\n Created Tithe Tx for Outbound Money %f to address %s ",(double)aTitheAmount, sAddrF.c_str());
				vecSend.push_back(recFoundation);
			}

			if (rcp.ipfshash.length() > 0)
			{
				if (rcp.ipfshash.length() != 64)
				{
					std::string sError = "";
					std::string sIPHash = SubmitToIPFS(FromQStringW(rcp.ipfshash), sError);
					if (!sError.empty())
					{
						QString qErr = "IPFS Attachment Failed.  " + ToQstring(sError);
						Q_EMIT message(tr("Send Coins"), qErr, CClientUIInterface::MSG_ERROR);
						return TransactionCreationFailed;
					}
					// At this point, the file is good, Biblepay can add it to IPFS and we know the size.  Now we can calculate the cost and add the PODS fee.
					int64_t nFileSize = GetFileSize(FromQStringW(rcp.ipfshash));
					double dCostPerByte = GetSporkDouble("ipfscostperbyte", .0002);
					CAmount aIPFSFee = dCostPerByte * nFileSize * COIN;
					const Consensus::Params& consensusParams = Params().GetConsensus();
					CScript spkFoundation = GetScriptForDestination(CBitcoinAddress(consensusParams.FoundationPODSAddress).Get());
		            CRecipient recFoundation = {spkFoundation, aIPFSFee, rcp.fForce, false, true, rcp.fPrayer, rcp.fRepent, FromQStringW(rcp.txtMessage), FromQStringW(rcp.txtRepent), FromQStringW(""), FromQStringW(rcp.ipfshash) };
					std::string sAddrF = PubKeyToAddress(spkFoundation);
					setAddress.insert(ToQstring(sAddrF));
            		++nAddresses;
					LogPrintf(" \r\n Created IPFS Fee vout for bbp %f to address %s ", (double)(aIPFSFee/COIN), sAddrF.c_str());
					vecSend.push_back(recFoundation);
					sMessages += "<PACK><MT>ATTACHMENT</MT><MK>OUT_TX</MK><MV>" + sIPHash + "</MV><ipfshash>" + sIPHash + "</ipfshash><ipfssize>" + RoundToString(nFileSize, 0) + "</ipfssize></PACK>";
					LogPrintf("Attaching %s ",sMessages.c_str());
				}
			}

			if (rcp.txtMessage.length() > 0 || rcp.txtRepent.length() > 0)
			{
				// Store Message or Prayer or IPFS Attachment in this TX : (R Andrews - 8/27/2018)
				std::string sMessageType = "MESSAGE";
				std::string sRepent = "";
				
				if (rcp.fPrayer)
				{
					sMessageType = "PRAYER";
				}
				else if (rcp.fRepent) 
				{
					sMessageType="REPENT"; 
					sRepent = FromQStringW(rcp.txtRepent);
				}
				else 
				{
					sMessageType = "MESSAGE";  
				}
				std::string sMessageKey  = "OUT_TX";
				sMessages += "<PACK><MT>" + sMessageType + "</MT><MK>" + sMessageKey + "</MK><MV>" + FromQStringW(rcp.txtMessage) + " " + sRepent + "</MV></PACK>";
			}
            total += rcp.amount;
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    CAmount nBalance = getBalance(coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        transaction.newPossibleKeyChange(wallet);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        std::string strFailReason;

        CWalletTx *newTx = transaction.getTransaction();
        CReserveKey *keyChange = transaction.getPossibleKeyChange();

        if(recipients[0].fUseInstantSend && total > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN){
            Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending values that high yet. Transactions are currently limited to %1 biblepay.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                         CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

		newTx->sTxMessageConveyed = sMessages;
		LogPrintf("Creating Tx Message %s ",newTx->sTxMessageConveyed.c_str());

	    // R ANDREWS - POG - 12/6/2018 - If this is a tithe, we must tack on the difficulty params:
		// bool fUseInstantSend=false, int iMinConfirms = 0, double dMinCoinAge = 0, CAmount caMinCoinAmount = 0);

		double dMinCoinAge = 0;
		CAmount caMinCoinAmount = 0;
		CAmount caMaxTitheAmount = 0;
		if (fTithed && fPOGEnabled)
		{
			TitheDifficultyParams tdp = GetTitheParams(chainActive.Tip());
			dMinCoinAge = tdp.min_coin_age;
			caMinCoinAmount = tdp.min_coin_amount;
			caMaxTitheAmount = tdp.max_tithe_amount;
		}
	
        bool fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl, true, recipients[0].inputType, 
			recipients[0].fUseInstantSend, 0, dMinCoinAge, caMinCoinAmount);

        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && fCreated)
            transaction.reassignAmounts(nChangePosRet);
		if (!fForce)
		{
			if (fPOGEnabled && fTithed && total > caMaxTitheAmount)
			{
				Q_EMIT message(tr("Send Coins"), tr("Your tithe exceeds the current maximum tithe for this difficulty level of %1 biblepay.").arg(caMaxTitheAmount/COIN),
								 CClientUIInterface::MSG_ERROR);
					return TransactionCreationFailed;
			}
		}

        if(recipients[0].fUseInstantSend) {
            if(newTx->GetValueOut() > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN) {
                Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending values that high yet. Transactions are currently limited to %1 biblepay.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                             CClientUIInterface::MSG_ERROR);
                return TransactionCreationFailed;
            }
            if(newTx->vin.size() > CTxLockRequest::WARN_MANY_INPUTS) {
                Q_EMIT message(tr("Send Coins"), tr("Used way too many inputs (>%1) for this InstantSend transaction, fees could be huge.").arg(CTxLockRequest::WARN_MANY_INPUTS),
                             CClientUIInterface::MSG_WARNING);
            }
        }


        if(!fCreated)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(strFailReason),
                         CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // reject absurdly high fee. (This can never happen because the
        // wallet caps the fee at maxTxFee. This merely serves as a
        // belt-and-suspenders check)
        if (nFeeRequired > maxTxFee)
            return AbsurdFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction &transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();
        QList<SendCoinsRecipient> recipients = transaction.getRecipients();

        Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
        {
            if (rcp.paymentRequest.IsInitialized())
            {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(make_pair(key, value));
            }
            else if (!rcp.message.isEmpty()) // Message from normal biblepay:URI (biblepay:XyZ...?message=example)
            {
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
            }
        }

        CReserveKey *keyChange = transaction.getPossibleKeyChange();
		if(!wallet->CommitTransaction(*newTx, *keyChange, recipients[0].fUseInstantSend ? NetMsgType::TXLOCKREQUEST : NetMsgType::TX))
            return TransactionCommitFailed;

        CTransaction* t = (CTransaction*)newTx;
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
    {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized())
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = CBitcoinAddress(strAddress).Get();
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end())
                {
                    wallet->SetAddressBook(dest, strLabel, "send");
                }
                else if (mi->second.name != strLabel)
                {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

RecentRequestsTableModel *WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!wallet->IsCrypted())
    {
        return Unencrypted;
    }
    else if(wallet->IsLocked(true))
    {
        return Locked;
    }
    else if (wallet->IsLocked())
    {
        return UnlockedForMixingOnly;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString &passphrase)
{
    if(encrypted)
    {
        // Encrypt
        return wallet->EncryptWallet(passphrase);
    }
    else
    {
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase, bool fMixing)
{
    if(locked)
    {
        // Lock
        return wallet->Lock(fMixing);
    }
    else
    {
        // Unlock
        return wallet->Unlock(passPhrase, fMixing);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString &filename)
{
    return BackupWallet(*wallet, filename.toLocal8Bit().data());
}

// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel, CCryptoKeyStore *wallet)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel, CWallet *wallet,
        const CTxDestination &address, const std::string &label, bool isMine,
        const std::string &purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(CBitcoinAddress(address).ToString());
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(QString, strPurpose),
                              Q_ARG(int, status));
}

static void NotifyTransactionChanged(WalletModel *walletmodel, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    Q_UNUSED(status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
}

static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyStatusChanged.connect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.connect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.connect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyStatusChanged.disconnect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.disconnect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.disconnect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock(bool fForMixingOnly)
{
    EncryptionStatus encStatusOld = getEncryptionStatus();

    // Wallet was completely locked
    bool was_locked = (encStatusOld == Locked);
    // Wallet was unlocked for mixing
    bool was_mixing = (encStatusOld == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing and now user requested to fully unlock it
    bool fMixingToFullRequested = !fForMixingOnly && was_mixing;

    if(was_locked || fMixingToFullRequested) {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock(fForMixingOnly);
    }

    EncryptionStatus encStatusNew = getEncryptionStatus();

    // Wallet was locked, user requested to unlock it for mixing and failed to do so
    bool fMixingUnlockFailed = fForMixingOnly && !(encStatusNew == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing, user requested to fully unlock it and failed
    bool fMixingToFullFailed = fMixingToFullRequested && !(encStatusNew == Unlocked);
    // If wallet is still locked, unlock failed or was cancelled, mark context as invalid
    bool fInvalid = (encStatusNew == Locked) || fMixingUnlockFailed || fMixingToFullFailed;
    // Wallet was not locked in any way or user tried to unlock it for mixing only and succeeded, keep it unlocked
    bool fKeepUnlocked = !was_locked || (fForMixingOnly && !fMixingUnlockFailed);

    return UnlockContext(this, !fInvalid, !fKeepUnlocked, was_mixing);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *wallet, bool valid, bool was_locked, bool was_mixing):
        wallet(wallet),
        valid(valid),
        was_locked(was_locked),
        was_mixing(was_mixing)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && (was_locked || was_mixing))
    {
        wallet->setWalletLocked(true, "", was_mixing);
    }
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.was_locked = false;
    rhs.was_mixing = false;
}

bool WalletModel::getPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    return wallet->GetPubKey(address, vchPubKeyOut);
}

bool WalletModel::havePrivKey(const CKeyID &address) const
{
    return wallet->HaveKey(address);
}

// returns a list of COutputs from COutPoints
void WalletModel::getOutputs(const std::vector<COutPoint>& vOutpoints, std::vector<COutput>& vOutputs)
{
    LOCK2(cs_main, wallet->cs_wallet);
    BOOST_FOREACH(const COutPoint& outpoint, vOutpoints)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true);
        vOutputs.push_back(out);
    }
}

bool WalletModel::isSpent(const COutPoint& outpoint) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsSpent(outpoint.hash, outpoint.n);
}

// AvailableCoins + LockedCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listCoins(std::map<QString, std::vector<COutput> >& mapCoins) const
{
    std::vector<COutput> vCoins;
    wallet->AvailableCoins(vCoins);

    LOCK2(cs_main, wallet->cs_wallet); // ListLockedCoins, mapWallet
    std::vector<COutPoint> vLockedCoins;
    wallet->ListLockedCoins(vLockedCoins);

    // add locked coins
    BOOST_FOREACH(const COutPoint& outpoint, vLockedCoins)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true);
        if (outpoint.n < out.tx->vout.size() && wallet->IsMine(out.tx->vout[outpoint.n]) == ISMINE_SPENDABLE)
            vCoins.push_back(out);
    }

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        COutput cout = out;

        while (wallet->IsChange(cout.tx->vout[cout.i]) && cout.tx->vin.size() > 0 && wallet->IsMine(cout.tx->vin[0]))
        {
            if (!wallet->mapWallet.count(cout.tx->vin[0].prevout.hash)) break;
            cout = COutput(&wallet->mapWallet[cout.tx->vin[0].prevout.hash], cout.tx->vin[0].prevout.n, 0, true);
        }

        CTxDestination address;
        if(!out.fSpendable || !ExtractDestination(cout.tx->vout[cout.i].scriptPubKey, address))
            continue;
        mapCoins[QString::fromStdString(CBitcoinAddress(address).ToString())].push_back(out);
    }
}

bool WalletModel::isLockedCoin(uint256 hash, unsigned int n) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsLockedCoin(hash, n);
}

void WalletModel::lockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->LockCoin(output);
}

void WalletModel::unlockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->UnlockCoin(output);
}

void WalletModel::listLockedCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->ListLockedCoins(vOutpts);
}

void WalletModel::loadReceiveRequests(std::vector<std::string>& vReceiveRequests)
{
    LOCK(wallet->cs_wallet);
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& item, wallet->mapAddressBook)
        BOOST_FOREACH(const PAIRTYPE(std::string, std::string)& item2, item.second.destdata)
            if (item2.first.size() > 2 && item2.first.substr(0,2) == "rr") // receive request
                vReceiveRequests.push_back(item2.second);
}

bool WalletModel::saveReceiveRequest(const std::string &sAddress, const int64_t nId, const std::string &sRequest)
{
    CTxDestination dest = CBitcoinAddress(sAddress).Get();

    std::stringstream ss;
    ss << nId;
    std::string key = "rr" + ss.str(); // "rr" prefix = "receive request" in destdata

    LOCK(wallet->cs_wallet);
    if (sRequest.empty())
        return wallet->EraseDestData(dest, key);
    else
        return wallet->AddDestData(dest, key, sRequest);
}
