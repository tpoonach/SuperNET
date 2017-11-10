
/******************************************************************************
 * Copyright © 2014-2017 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/
//
//  LP_cache.c
//  marketmaker
//

cJSON *LP_transaction_fromdata(struct iguana_info *coin,bits256 txid,uint8_t *serialized,int32_t len)
{
    uint8_t *extraspace; cJSON *txobj; char str[65],str2[65]; struct iguana_msgtx msgtx; bits256 checktxid;
    extraspace = calloc(1,4000000);
    memset(&msgtx,0,sizeof(msgtx));
    txobj = bitcoin_data2json(coin->taddr,coin->pubtype,coin->p2shtype,coin->isPoS,coin->height,&checktxid,&msgtx,extraspace,4000000,serialized,len,0,0,coin->zcash);
    //printf("TX.(%s) match.%d\n",jprint(txobj,0),bits256_cmp(txid,checktxid));
    free(extraspace);
    if ( bits256_cmp(txid,checktxid) != 0 )
    {
        printf("%s LP_transaction_fromdata mismatched txid %s vs %s\n",coin->symbol,bits256_str(str,txid),bits256_str(str2,checktxid));
        free_json(txobj);
        txobj = 0;
    }
    return(txobj);
}

cJSON *LP_cache_transaction(struct iguana_info *coin,bits256 txid,uint8_t *serialized,int32_t len)
{
    cJSON *txobj; struct LP_transaction *tx;
    if ( (txobj= LP_transaction_fromdata(coin,txid,serialized,len)) != 0 )
    {
        if ( (tx= LP_transactionfind(coin,txid)) == 0 || tx->serialized == 0 )
        {
            txobj = LP_transactioninit(coin,txid,0,txobj);
            LP_transactioninit(coin,txid,1,txobj);
            tx = LP_transactionfind(coin,txid);
        }
        if ( tx != 0 )
        {
            tx->serialized = serialized;
            tx->len = len;
        }
        else
        {
            char str[65]; printf("unexpected couldnt find tx %s %s\n",coin->symbol,bits256_str(str,txid));
            free(serialized);
        }
    }
    return(txobj);
}

int32_t LP_SPV_load(struct iguana_info *coin,bits256 txid,int32_t height)
{
    return(-1);
}

void LP_SPV_store(struct iguana_info *coin,char *coinaddr,bits256 txid,int32_t height)
{
    struct LP_transaction *tx = 0;
    if ( strcmp(coin->smartaddr,coinaddr) == 0 && (tx= LP_transactionfind(coin,txid)) != 0 && tx->serialized != 0 )
    {
        //char str[65]; printf("store %s %s.[%d]\n",coin->symbol,bits256_str(str,txid),tx->len);
    } //else printf("skip SPV store for (%s) tx.%p\n",coinaddr,tx);
}

bits256 iguana_merkle(bits256 *tree,int32_t txn_count)
{
    int32_t i,n=0,prev; uint8_t serialized[sizeof(bits256) * 2];
    if ( txn_count == 1 )
        return(tree[0]);
    prev = 0;
    while ( txn_count > 1 )
    {
        if ( (txn_count & 1) != 0 )
            tree[prev + txn_count] = tree[prev + txn_count-1], txn_count++;
        n += txn_count;
        for (i=0; i<txn_count; i+=2)
        {
            iguana_rwbignum(1,serialized,sizeof(*tree),tree[prev + i].bytes);
            iguana_rwbignum(1,&serialized[sizeof(*tree)],sizeof(*tree),tree[prev + i + 1].bytes);
            tree[n + (i >> 1)] = bits256_doublesha256(0,serialized,sizeof(serialized));
        }
        prev = n;
        txn_count >>= 1;
    }
    return(tree[n]);
}

bits256 validate_merkle(int32_t pos,bits256 txid,cJSON *proofarray,int32_t proofsize)
{
    int32_t i; uint8_t serialized[sizeof(bits256) * 2]; bits256 hash,proof;
    hash = txid;
    for (i=0; i<proofsize; i++)
    {
        proof = jbits256i(proofarray,i);
        if ( (pos & 1) == 0 )
        {
            iguana_rwbignum(1,&serialized[0],sizeof(hash),hash.bytes);
            iguana_rwbignum(1,&serialized[sizeof(hash)],sizeof(proof),proof.bytes);
        }
        else
        {
            iguana_rwbignum(1,&serialized[0],sizeof(proof),proof.bytes);
            iguana_rwbignum(1,&serialized[sizeof(hash)],sizeof(hash),hash.bytes);
        }
        hash = bits256_doublesha256(0,serialized,sizeof(serialized));
        pos >>= 1;
    }
    return(hash);
}

bits256 LP_merkleroot(struct iguana_info *coin,struct electrum_info *ep,int32_t height)
{
    cJSON *hdrobj; bits256 merkleroot;
    memset(merkleroot.bytes,0,sizeof(merkleroot));
    if ( coin->cachedmerkleheight == height )
        return(coin->cachedmerkle);
    if ( (hdrobj= electrum_getheader(coin->symbol,ep,&hdrobj,height)) != 0 )
    {
        if ( jobj(hdrobj,"merkle_root") != 0 )
        {
            merkleroot = jbits256(hdrobj,"merkle_root");
            if ( bits256_nonz(merkleroot) != 0 )
            {
                coin->cachedmerkle = merkleroot;
                coin->cachedmerkleheight = height;
            }
        }
        free_json(hdrobj);
    } else printf("couldnt get header for ht.%d\n",height);
    return(merkleroot);
}

int32_t LP_merkleproof(struct iguana_info *coin,char *coinaddr,struct electrum_info *ep,bits256 txid,int32_t height)
{
    cJSON *merkobj,*merkles; bits256 roothash,merkleroot; int32_t retval,m,SPV = 0;
    if ( strcmp(coin->smartaddr,coinaddr) == 0 && (retval= LP_SPV_load(coin,txid,height)) > 0 )
        return(retval);
    if ( (merkobj= electrum_getmerkle(coin->symbol,ep,&merkobj,txid,height)) != 0 )
    {
        char str[65],str2[65],str3[65];
        SPV = -1;
        memset(roothash.bytes,0,sizeof(roothash));
        if ( (merkles= jarray(&m,merkobj,"merkle")) != 0 )
        {
            roothash = validate_merkle(jint(merkobj,"pos"),txid,merkles,m);
            merkleroot = LP_merkleroot(coin,ep,height);
            if ( bits256_nonz(merkleroot) != 0 )
            {
                if ( bits256_cmp(merkleroot,roothash) == 0 )
                {
                    SPV = height;
                    LP_SPV_store(coin,coinaddr,txid,height);
                    //printf("validated MERK %s ht.%d -> %s root.(%s)\n",bits256_str(str,up->U.txid),up->U.height,jprint(merkobj,0),bits256_str(str2,roothash));
                }
                else printf("ERROR MERK %s ht.%d -> %s root.(%s) vs %s\n",bits256_str(str,txid),height,jprint(merkobj,0),bits256_str(str2,roothash),bits256_str(str3,merkleroot));
            } else SPV = 0;
        }
        if ( SPV < 0 )
        {
            printf("MERKLE DIDNT VERIFY.%s %s ht.%d (%s)\n",coin->symbol,bits256_str(str,txid),height,jprint(merkobj,0));
            if ( jobj(merkobj,"error") != 0 )
                SPV = 0; // try again later
        }
        free_json(merkobj);
    }
    return(SPV);
}

char *LP_unspents_filestr(char *symbol,char *addr)
{
    char fname[1024]; long fsize;
    sprintf(fname,"%s/UNSPENTS/%s_%s",GLOBAL_DBDIR,symbol,addr), OS_portable_path(fname);
    return(OS_filestr(&fsize,fname));
}

void LP_unspents_cache(char *symbol,char *addr,char *arraystr,int32_t updatedflag)
{
    char fname[1024]; FILE *fp=0;
    sprintf(fname,"%s/UNSPENTS/%s_%s",GLOBAL_DBDIR,symbol,addr), OS_portable_path(fname);
    //printf("unspents cache.(%s) for %s %s, updated.%d\n",fname,symbol,addr,updatedflag);
    if ( updatedflag == 0 && (fp= fopen(fname,"rb")) == 0 )
        updatedflag = 1;
    else if ( fp != 0 )
        fclose(fp);
    if ( updatedflag != 0 && (fp= fopen(fname,"wb")) != 0 )
    {
        fwrite(arraystr,1,strlen(arraystr),fp);
        fclose(fp);
    }
}

uint64_t LP_unspents_load(char *symbol,char *addr)
{
    char *arraystr; uint64_t balance = 0; int32_t i,n; cJSON *retjson,*item; struct iguana_info *coin;
    if ( (coin= LP_coinfind(symbol)) != 0 )
    {
        if ( (arraystr= LP_unspents_filestr(symbol,addr)) != 0 )
        {
            if ( (retjson= cJSON_Parse(arraystr)) != 0 )
            {
                //printf("PROCESS UNSPENTS %s\n",arraystr);
                if ( (n= cJSON_GetArraySize(retjson)) > 0 )
                {
                    for (i=0; i<n; i++)
                    {
                        item = jitem(retjson,i);
                        balance += j64bits(item,"value");
                    }
                }
                electrum_process_array(coin,coin->electrum,coin->smartaddr,retjson,1);
                free_json(retjson);
            }
            free(arraystr);
        }
    }
    return(balance);
}


