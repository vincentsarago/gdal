/******************************************************************************
 *
 * Project:  PLMosaic driver
 * Purpose:  PLMosaic driver
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2015, Planet Labs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_http.h"
#include "cpl_minixml.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "gdal_priv.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"

#include "ogrgeojsonreader.h"

CPL_CVSID("$Id$")

// g++ -fPIC -g -Wall frmts/plmosaic/*.cpp -shared -o gdal_PLMOSAIC.so -Iport -Igcore -Iogr -Iogr/ogrsf_frmts -Iogr/ogrsf_frmts/geojson/libjson -L. -lgdal

#define GM_ORIGIN  -20037508.340
#define GM_ZOOM_0  ((2 * -(GM_ORIGIN)) / 256)

/************************************************************************/
/* ==================================================================== */
/*                           PLMosaicDataset                            */
/* ==================================================================== */
/************************************************************************/

class PLLinkedDataset;
class PLLinkedDataset
{
public:
    CPLString            osKey;
    GDALDataset         *poDS;
    PLLinkedDataset       *psPrev;
    PLLinkedDataset       *psNext;

                        PLLinkedDataset() : poDS(nullptr), psPrev(nullptr), psNext(nullptr) {}
};

class PLMosaicRasterBand;

class PLMosaicDataset : public GDALPamDataset
{
    friend class PLMosaicRasterBand;

        int                     bMustCleanPersistent;
        CPLString               osCachePathRoot;
        int                     bTrustCache;
        CPLString               osBaseURL;
        CPLString               osAPIKey;
        CPLString               osMosaic;
        char                   *pszWKT;
        int                     nQuadSize;
        CPLString               osQuadPattern;
        CPLString               osQuadsURL;
        int                     bHasGeoTransform;
        double                  adfGeoTransform[6];
        int                     nZoomLevel;
        int                     bUseTMSForMain;
        GDALDataset            *poTMSDS;

        int                     nCacheMaxSize;
        std::map<CPLString, PLLinkedDataset*> oMapLinkedDatasets;
        PLLinkedDataset        *psHead;
        PLLinkedDataset        *psTail;
        void                    FlushDatasetsCache();
        CPLString               GetMosaicCachePath();
        void                    CreateMosaicCachePathIfNecessary();

        int                     nLastMetaTileX;
        int                     nLastMetaTileY;
        CPLString               osLastQuadInformation;
        CPLString               osLastQuadSceneInformation;
        CPLString               osLastRetGetLocationInfo;
        const char             *GetLocationInfo(int nPixel, int nLine);

        char                  **GetBaseHTTPOptions();
        CPLHTTPResult          *Download(const char* pszURL,
                                         int bQuiet404Error = FALSE);
        json_object            *RunRequest(const char* pszURL,
                                           int bQuiet404Error = FALSE);
        int                     OpenMosaic();
        int                     ListSubdatasets();

        CPLString               formatTileName(int tile_x, int tile_y);
        void                    InsertNewDataset(CPLString osKey, GDALDataset* poDS);
        GDALDataset*            OpenAndInsertNewDataset(CPLString osTmpFilename,
                                                        CPLString osTilename);

  public:
                PLMosaicDataset();
    virtual  ~PLMosaicDataset();

    static int Identify( GDALOpenInfo * poOpenInfo );
    static GDALDataset  *Open( GDALOpenInfo * );

    virtual CPLErr  IRasterIO( GDALRWFlag eRWFlag,
                               int nXOff, int nYOff, int nXSize, int nYSize,
                               void * pData, int nBufXSize, int nBufYSize,
                               GDALDataType eBufType,
                               int nBandCount, int *panBandMap,
                               GSpacing nPixelSpace, GSpacing nLineSpace,
                               GSpacing nBandSpace,
                               GDALRasterIOExtraArg* psExtraArg) override;

    virtual void FlushCache(void) override;

    virtual const char *GetProjectionRef() override;
    virtual CPLErr      GetGeoTransform(double* padfGeoTransform) override;

    GDALDataset        *GetMetaTile(int tile_x, int tile_y);
};

/************************************************************************/
/* ==================================================================== */
/*                         PLMosaicRasterBand                           */
/* ==================================================================== */
/************************************************************************/

class PLMosaicRasterBand : public GDALRasterBand
{
    friend class PLMosaicDataset;

  public:

                PLMosaicRasterBand( PLMosaicDataset * poDS, int nBand,
                                    GDALDataType eDataType );

    virtual CPLErr          IReadBlock( int, int, void * ) override;
    virtual CPLErr          IRasterIO( GDALRWFlag eRWFlag,
                                  int nXOff, int nYOff, int nXSize, int nYSize,
                                  void * pData, int nBufXSize, int nBufYSize,
                                  GDALDataType eBufType,
                                  GSpacing nPixelSpace, GSpacing nLineSpace,
                                  GDALRasterIOExtraArg* psExtraArg) override;

    virtual const char     *GetMetadataItem( const char* pszName,
                                             const char * pszDomain = "" ) override;

    virtual GDALColorInterp GetColorInterpretation() override;

    virtual int             GetOverviewCount() override;
    virtual GDALRasterBand* GetOverview(int iOvrLevel) override;
};

/************************************************************************/
/*                        PLMosaicRasterBand()                          */
/************************************************************************/

PLMosaicRasterBand::PLMosaicRasterBand( PLMosaicDataset *poDSIn, int nBandIn,
                                        GDALDataType eDataTypeIn )

{
    eDataType = eDataTypeIn;
    nBlockXSize = 256;
    nBlockYSize = 256;

    poDS = poDSIn;
    nBand = nBandIn;

    if( eDataType == GDT_UInt16 )
    {
        if( nBand <= 3 )
            SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr PLMosaicRasterBand::IReadBlock( int nBlockXOff, int nBlockYOff,
                                       void *pImage )
{
    PLMosaicDataset* poMOSDS = reinterpret_cast<PLMosaicDataset *>( poDS );

#ifdef DEBUG_VERBOSE
    CPLDebug("PLMOSAIC", "IReadBlock(band=%d, x=%d, y=%d)",
             nBand, nBlockYOff, nBlockYOff);
#endif

    if( poMOSDS->bUseTMSForMain && poMOSDS->poTMSDS )
        return poMOSDS->poTMSDS->GetRasterBand(nBand)->ReadBlock(nBlockXOff, nBlockYOff,
                                                                pImage);

    const int bottom_yblock = (nRasterYSize - nBlockYOff * nBlockYSize) / nBlockYSize - 1;

    const int meta_tile_x = (nBlockXOff * nBlockXSize) / poMOSDS->nQuadSize;
    const int meta_tile_y = (bottom_yblock * nBlockYSize) / poMOSDS->nQuadSize;
    const int sub_tile_x = nBlockXOff % (poMOSDS->nQuadSize / nBlockXSize);
    const int sub_tile_y = nBlockYOff % (poMOSDS->nQuadSize / nBlockYSize);

    GDALDataset *poMetaTileDS = poMOSDS->GetMetaTile(meta_tile_x, meta_tile_y);
    if( poMetaTileDS == nullptr )
    {
        memset(pImage, 0,
               nBlockXSize * nBlockYSize * (GDALGetDataTypeSize(eDataType)/8));
        return CE_None;
    }

    return poMetaTileDS->GetRasterBand(nBand)->
                RasterIO( GF_Read,
                        sub_tile_x * nBlockXSize,
                        sub_tile_y * nBlockYSize,
                        nBlockXSize,
                        nBlockYSize,
                        pImage, nBlockXSize, nBlockYSize,
                        eDataType, 0, 0, nullptr);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr PLMosaicRasterBand::IRasterIO( GDALRWFlag eRWFlag,
                                         int nXOff, int nYOff, int nXSize, int nYSize,
                                         void * pData, int nBufXSize, int nBufYSize,
                                         GDALDataType eBufType,
                                         GSpacing nPixelSpace, GSpacing nLineSpace,
                                         GDALRasterIOExtraArg* psExtraArg )
{
    PLMosaicDataset* poMOSDS = reinterpret_cast<PLMosaicDataset *>( poDS );
    if( poMOSDS->bUseTMSForMain && poMOSDS->poTMSDS )
        return poMOSDS->poTMSDS->GetRasterBand(nBand)->RasterIO(
                                         eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize, eBufType,
                                         nPixelSpace, nLineSpace, psExtraArg );

    return GDALRasterBand::IRasterIO( eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize, eBufType,
                                         nPixelSpace, nLineSpace, psExtraArg );
}

/************************************************************************/
/*                         GetMetadataItem()                            */
/************************************************************************/

const char* PLMosaicRasterBand::GetMetadataItem( const char* pszName,
                                                 const char* pszDomain )
{
    int nPixel, nLine;
    if( pszName != nullptr && pszDomain != nullptr && EQUAL(pszDomain, "LocationInfo") &&
        sscanf(pszName, "Pixel_%d_%d", &nPixel, &nLine) == 2 )
    {
        PLMosaicDataset* poMOSDS = reinterpret_cast<PLMosaicDataset *>( poDS );
        return poMOSDS->GetLocationInfo(nPixel, nLine);
    }

    return GDALRasterBand::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                         GetOverviewCount()                           */
/************************************************************************/

int PLMosaicRasterBand::GetOverviewCount()
{
    PLMosaicDataset *poGDS = reinterpret_cast<PLMosaicDataset *>( poDS );
    if( !poGDS->poTMSDS )
        return 0;

    return poGDS->poTMSDS->GetRasterBand(1)->GetOverviewCount();
}

/************************************************************************/
/*                            GetOverview()                             */
/************************************************************************/

GDALRasterBand* PLMosaicRasterBand::GetOverview(int iOvrLevel)
{
    PLMosaicDataset *poGDS = reinterpret_cast<PLMosaicDataset *>( poDS );
    if (iOvrLevel < 0 || iOvrLevel >= GetOverviewCount())
        return nullptr;

    poGDS->CreateMosaicCachePathIfNecessary();

    return poGDS->poTMSDS->GetRasterBand(nBand)->GetOverview(iOvrLevel);
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp PLMosaicRasterBand::GetColorInterpretation()
{
    switch( nBand )
    {
        case 1:
            return GCI_RedBand;
        case 2:
            return GCI_GreenBand;
        case 3:
            return GCI_BlueBand;
        case 4:
            return GCI_AlphaBand;
        default:
            CPLAssert(false);
            return GCI_GrayIndex;
    }
}

/************************************************************************/
/* ==================================================================== */
/*                           PLMosaicDataset                            */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                        PLMosaicDataset()                            */
/************************************************************************/

PLMosaicDataset::PLMosaicDataset() :
    bMustCleanPersistent(FALSE),
    bTrustCache(FALSE),
    pszWKT(nullptr),
    nQuadSize(0),
    bHasGeoTransform(FALSE),
    nZoomLevel(0),
    bUseTMSForMain(FALSE),
    poTMSDS(nullptr),
    nCacheMaxSize(10),
    psHead(nullptr),
    psTail(nullptr),
    nLastMetaTileX(-1),
    nLastMetaTileY(-1)
{
    adfGeoTransform[0] = 0;
    adfGeoTransform[1] = 1;
    adfGeoTransform[2] = 0;
    adfGeoTransform[3] = 0;
    adfGeoTransform[4] = 0;
    adfGeoTransform[5] = 1;

    SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    osCachePathRoot = CPLGetPath(CPLGenerateTempFilename(""));
}

/************************************************************************/
/*                         ~PLMosaicDataset()                           */
/************************************************************************/

PLMosaicDataset::~PLMosaicDataset()

{
    FlushCache();
    CPLFree(pszWKT);
    delete poTMSDS;
    if (bMustCleanPersistent)
    {
        char** papszOptions
            = CSLSetNameValue(nullptr, "CLOSE_PERSISTENT",
                              CPLSPrintf("PLMOSAIC:%p", this));
        CPLHTTPDestroyResult( CPLHTTPFetch( osBaseURL, papszOptions) );
        CSLDestroy(papszOptions);
    }
}

/************************************************************************/
/*                      FlushDatasetsCache()                            */
/************************************************************************/

void PLMosaicDataset::FlushDatasetsCache()
{
    for( PLLinkedDataset* psIter = psHead; psIter != nullptr;  )
    {
        PLLinkedDataset* psNext = psIter->psNext;
        if( psIter->poDS )
            GDALClose(psIter->poDS);
        delete psIter;
        psIter = psNext;
    }
    psHead = nullptr;
    psTail = nullptr;
    oMapLinkedDatasets.clear();
}

/************************************************************************/
/*                            FlushCache()                              */
/************************************************************************/

void PLMosaicDataset::FlushCache()
{
    FlushDatasetsCache();

    nLastMetaTileX = -1;
    nLastMetaTileY = -1;
    osLastQuadInformation = "";
    osLastQuadSceneInformation = "";
    osLastRetGetLocationInfo = "";

    GDALDataset::FlushCache();
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int PLMosaicDataset::Identify( GDALOpenInfo * poOpenInfo )

{
    return STARTS_WITH_CI(poOpenInfo->pszFilename, "PLMOSAIC:");
}

/************************************************************************/
/*                          GetBaseHTTPOptions()                         */
/************************************************************************/

char** PLMosaicDataset::GetBaseHTTPOptions()
{
    bMustCleanPersistent = TRUE;

    char** papszOptions
        = CSLAddString(nullptr, CPLSPrintf("PERSISTENT=PLMOSAIC:%p", this));
    /* Use basic auth, rather than Authorization headers since curl would forward it to S3 */
    papszOptions = CSLAddString(papszOptions, CPLSPrintf("USERPWD=%s:", osAPIKey.c_str()));

    return papszOptions;
}

/************************************************************************/
/*                               Download()                             */
/************************************************************************/

CPLHTTPResult* PLMosaicDataset::Download(const char* pszURL,
                                         int bQuiet404Error)
{
    char** papszOptions = CSLAddString(GetBaseHTTPOptions(), nullptr);
    CPLHTTPResult *psResult = nullptr;
    if( STARTS_WITH(osBaseURL, "/vsimem/") &&
        STARTS_WITH(pszURL, "/vsimem/") )
    {
        CPLDebug("PLSCENES", "Fetching %s", pszURL);
        psResult = reinterpret_cast<CPLHTTPResult *>(
            CPLCalloc( 1, sizeof( CPLHTTPResult ) ) );
        vsi_l_offset nDataLength = 0;
        CPLString osURL(pszURL);
        if( osURL.back() == '/' )
            osURL.resize(osURL.size()-1);
        GByte* pabyBuf = VSIGetMemFileBuffer(osURL, &nDataLength, FALSE);
        if( pabyBuf )
        {
            psResult->pabyData = reinterpret_cast<GByte *>(
                VSIMalloc(1 + static_cast<size_t>( nDataLength ) ) );
            if( psResult->pabyData )
            {
                memcpy(psResult->pabyData, pabyBuf, static_cast<size_t>( nDataLength ) );
                psResult->pabyData[nDataLength] = 0;
                psResult->nDataLen = static_cast<int>( nDataLength );
            }
        }
        else
        {
            psResult->pszErrBuf =
                CPLStrdup(CPLSPrintf("Error 404. Cannot find %s", pszURL));
        }
    }
    else
    {
        if( bQuiet404Error )
            CPLPushErrorHandler(CPLQuietErrorHandler);
        psResult = CPLHTTPFetch( pszURL, papszOptions);
        if( bQuiet404Error )
            CPLPopErrorHandler();
    }
    CSLDestroy(papszOptions);

    if( psResult->pszErrBuf != nullptr )
    {
        if( !(bQuiet404Error && strstr(psResult->pszErrBuf, "404")) )
        {
            CPLError( CE_Failure, CPLE_AppDefined, "%s",
                      psResult->pabyData ? reinterpret_cast<const char*>(
                          psResult->pabyData ) :
                      psResult->pszErrBuf );
        }
        CPLHTTPDestroyResult(psResult);
        return nullptr;
    }

    if( psResult->pabyData == nullptr )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Empty content returned by server");
        CPLHTTPDestroyResult(psResult);
        return nullptr;
    }

    return psResult;
}

/************************************************************************/
/*                               RunRequest()                           */
/************************************************************************/

json_object* PLMosaicDataset::RunRequest(const char* pszURL,
                                         int bQuiet404Error)
{
    CPLHTTPResult * psResult = Download(pszURL, bQuiet404Error);
    if( psResult == nullptr )
    {
        return nullptr;
    }

    json_object* poObj = nullptr;
    const char* pszText = reinterpret_cast<const char*>(psResult->pabyData);
    if( !OGRJSonParse(pszText, &poObj, true) )
    {
        CPLHTTPDestroyResult(psResult);
        return nullptr;
    }

    CPLHTTPDestroyResult(psResult);

    if( json_object_get_type(poObj) != json_type_object )
    {
        CPLError( CE_Failure, CPLE_AppDefined, "Return is not a JSON dictionary");
        json_object_put(poObj);
        poObj = nullptr;
    }

    return poObj;
}

/************************************************************************/
/*                           PLMosaicGetParameter()                     */
/************************************************************************/

static CPLString PLMosaicGetParameter( GDALOpenInfo * poOpenInfo,
                                       char** papszOptions,
                                       const char* pszName,
                                       const char* pszDefaultVal )
{
    return CSLFetchNameValueDef( papszOptions, pszName,
        CSLFetchNameValueDef( poOpenInfo->papszOpenOptions, pszName,
                              pszDefaultVal ));
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *PLMosaicDataset::Open( GDALOpenInfo * poOpenInfo )

{
    if (!Identify(poOpenInfo) )
        return nullptr;

    PLMosaicDataset* poDS = new PLMosaicDataset();

    poDS->osBaseURL = CPLGetConfigOption("PL_URL", "https://api.planet.com/v0/mosaics/");

    char** papszOptions = CSLTokenizeStringComplex(
            poOpenInfo->pszFilename+strlen("PLMosaic:"), ",", TRUE, FALSE );
    for( char** papszIter = papszOptions; papszIter && *papszIter; papszIter ++ )
    {
        char* pszKey = nullptr;
        const char* pszValue = CPLParseNameValue(*papszIter, &pszKey);
        if( pszValue != nullptr )
        {
            if( !EQUAL(pszKey, "api_key") &&
                !EQUAL(pszKey, "mosaic") &&
                !EQUAL(pszKey, "cache_path") &&
                !EQUAL(pszKey, "trust_cache") &&
                !EQUAL(pszKey, "use_tiles") )
            {
                CPLError(CE_Failure, CPLE_NotSupported, "Unsupported option %s", pszKey);
                CPLFree(pszKey);
                delete poDS;
                CSLDestroy(papszOptions);
                return nullptr;
            }
            CPLFree(pszKey);
        }
    }

    poDS->osAPIKey = PLMosaicGetParameter(poOpenInfo, papszOptions, "api_key",
                                          CPLGetConfigOption("PL_API_KEY",""));

    if( poDS->osAPIKey.empty() )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Missing PL_API_KEY configuration option or API_KEY open option");
        delete poDS;
        CSLDestroy(papszOptions);
        return nullptr;
    }

    poDS->osMosaic = PLMosaicGetParameter(poOpenInfo, papszOptions, "mosaic", "");

    poDS->osCachePathRoot = PLMosaicGetParameter(poOpenInfo, papszOptions, "cache_path",
                                          CPLGetConfigOption("PL_CACHE_PATH",""));

    poDS->bTrustCache = CPLTestBool(PLMosaicGetParameter(
                        poOpenInfo, papszOptions, "trust_cache", "FALSE"));

    poDS->bUseTMSForMain = CPLTestBool(PLMosaicGetParameter(
                        poOpenInfo, papszOptions, "use_tiles", "FALSE"));

    CSLDestroy(papszOptions);
    papszOptions = nullptr;

    if( !poDS->osMosaic.empty() )
    {
        if( !poDS->OpenMosaic() )
        {
            delete poDS;
            poDS = nullptr;
        }
    }
    else
    {
        if( !poDS->ListSubdatasets() )
        {
            delete poDS;
            poDS = nullptr;
        }
        else
        {
            char** papszMD = poDS->GetMetadata("SUBDATASETS");
            if( CSLCount(papszMD) == 2 )
            {
                const CPLString osOldFilename(poOpenInfo->pszFilename);
                const CPLString osMosaicConnectionString
                    = CSLFetchNameValue(papszMD, "SUBDATASET_1_NAME");
                delete poDS;
                GDALOpenInfo oOpenInfo(osMosaicConnectionString.c_str(), GA_ReadOnly);
                oOpenInfo.papszOpenOptions = poOpenInfo->papszOpenOptions;
                poDS = reinterpret_cast<PLMosaicDataset *>( Open(&oOpenInfo) );
                if( poDS )
                    poDS->SetDescription(osOldFilename);
            }
        }
    }

    if( poDS )
        poDS->SetPamFlags(0);

    return poDS;
}

/************************************************************************/
/*                           ReplaceSubString()                         */
/************************************************************************/

static void ReplaceSubString(CPLString &osTarget,
                             CPLString osPattern,
                             CPLString osReplacement)

{
    // Assumes only one occurrence of osPattern.
    size_t pos = osTarget.find(osPattern);
    if( pos == CPLString::npos )
        return;

    osTarget.replace(pos, osPattern.size(), osReplacement);
}

/************************************************************************/
/*                            GetMosaicCachePath()                      */
/************************************************************************/

CPLString PLMosaicDataset::GetMosaicCachePath()
{
    if( !osCachePathRoot.empty() )
    {
        const CPLString osCachePath(
            CPLFormFilename(osCachePathRoot, "plmosaic_cache", nullptr));
        const CPLString osMosaicPath(
            CPLFormFilename(osCachePath, osMosaic, nullptr));

        return osMosaicPath;
    }
    return "";
}

/************************************************************************/
/*                     CreateMosaicCachePathIfNecessary()               */
/************************************************************************/

void PLMosaicDataset::CreateMosaicCachePathIfNecessary()
{
    if( !osCachePathRoot.empty() )
    {
        const CPLString osCachePath(
            CPLFormFilename(osCachePathRoot, "plmosaic_cache", nullptr));
        const CPLString osMosaicPath(
            CPLFormFilename(osCachePath, osMosaic, nullptr));

        VSIStatBufL sStatBuf;
        if( VSIStatL(osMosaicPath, &sStatBuf) != 0 )
        {
            CPLPushErrorHandler(CPLQuietErrorHandler);
            VSIMkdir(osCachePathRoot, 0755);
            VSIMkdir(osCachePath, 0755);
            VSIMkdir(osMosaicPath, 0755);
            CPLPopErrorHandler();
        }
    }
}

/************************************************************************/
/*                               OpenMosaic()                           */
/************************************************************************/

int PLMosaicDataset::OpenMosaic()
{
    CPLString osURL(osBaseURL);
    if( osURL.back() != '/' )
        osURL += '/';
    osURL += osMosaic;
    json_object* poObj = RunRequest(osURL);
    if( poObj == nullptr )
    {
        return FALSE;
    }

    json_object* poCoordinateSystem = CPL_json_object_object_get(poObj, "coordinate_system");
    json_object* poDataType = CPL_json_object_object_get(poObj, "datatype");
    json_object* poQuadPattern = CPL_json_object_object_get(poObj, "quad_pattern");
    json_object* poQuadSize = CPL_json_object_object_get(poObj, "quad_size");
    json_object* poResolution = CPL_json_object_object_get(poObj, "resolution");
    json_object* poLinks = CPL_json_object_object_get(poObj, "links");
    json_object* poLinksQuads = nullptr;
    json_object* poLinksTiles = nullptr;
    if( poLinks != nullptr && json_object_get_type(poLinks) == json_type_object )
    {
        poLinksQuads = CPL_json_object_object_get(poLinks, "quads");
        poLinksTiles = CPL_json_object_object_get(poLinks, "tiles");
    }
    if( poCoordinateSystem == nullptr || json_object_get_type(poCoordinateSystem) != json_type_string ||
        poDataType == nullptr || json_object_get_type(poDataType) != json_type_string ||
        poQuadPattern == nullptr || json_object_get_type(poQuadPattern) != json_type_string ||
        poQuadSize == nullptr || json_object_get_type(poQuadSize) != json_type_int ||
        poResolution == nullptr || (json_object_get_type(poResolution) != json_type_int &&
                                 json_object_get_type(poResolution) != json_type_double) ||
        poLinksQuads == nullptr || json_object_get_type(poLinksQuads) != json_type_string )
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Missing required parameter");
        json_object_put(poObj);
        return FALSE;
    }

    const char* pszSRS = json_object_get_string(poCoordinateSystem);
    if( !EQUAL(pszSRS, "EPSG:3857") )
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Unsupported coordinate_system = %s",
                 pszSRS);
        json_object_put(poObj);
        return FALSE;
    }

    OGRSpatialReference oSRS;
    oSRS.SetFromUserInput(pszSRS);
    oSRS.exportToWkt(&pszWKT);

    GDALDataType eDT = GDT_Unknown;
    const char* pszDataType = json_object_get_string(poDataType);
    if( EQUAL(pszDataType, "byte") )
        eDT = GDT_Byte;
    else if( EQUAL(pszDataType, "uint16") )
        eDT = GDT_UInt16;
    else if( EQUAL(pszDataType, "int16") )
        eDT = GDT_Int16;
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Unsupported data_type = %s",
                 pszDataType);
        json_object_put(poObj);
        return FALSE;
    }

    if( bUseTMSForMain && eDT != GDT_Byte )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Cannot use tile API for full resolution data on non Byte mosaic");
        bUseTMSForMain = FALSE;
    }

    nQuadSize = json_object_get_int(poQuadSize);
    if( nQuadSize <= 0 || (nQuadSize % 256) != 0 )
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Unsupported quad_size = %d",
                 nQuadSize);
        json_object_put(poObj);
        return FALSE;
    }

    const double dfResolution = json_object_get_double(poResolution);
    if( EQUAL(pszSRS, "EPSG:3857") )
    {
        double dfZoomLevel = log(GM_ZOOM_0 / dfResolution)/log(2.0);
        nZoomLevel = static_cast<int>( dfZoomLevel + 0.1 );
        if( fabs(dfZoomLevel - nZoomLevel) > 1e-5 )
        {
            CPLError(CE_Failure, CPLE_NotSupported, "Unsupported resolution = %.12g",
                    dfResolution);
            json_object_put(poObj);
            return FALSE;
        }
        bHasGeoTransform = TRUE;
        adfGeoTransform[0] = GM_ORIGIN;
        adfGeoTransform[1] = dfResolution;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = -GM_ORIGIN;
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = -dfResolution;
        nRasterXSize = static_cast<int>( 2 * -GM_ORIGIN / dfResolution + 0.5 );
        nRasterYSize = nRasterXSize;
    }

    const char* pszQuadPattern = json_object_get_string(poQuadPattern);
    if( strstr(pszQuadPattern, "{tilex:") == nullptr ||
        strstr(pszQuadPattern, "{tiley:") == nullptr )
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid quad_pattern = %s",
                 pszQuadPattern);
        json_object_put(poObj);
        return FALSE;
    }
    osQuadPattern = pszQuadPattern;
    osQuadsURL = json_object_get_string(poLinksQuads);

    // Use WMS/TMS driver for overviews (only for byte)
    if( eDT == GDT_Byte && EQUAL(pszSRS, "EPSG:3857") &&
        poLinksTiles != nullptr && json_object_get_type(poLinksTiles) == json_type_string )
    {
        const char* pszLinksTiles = json_object_get_string(poLinksTiles);
        if( strstr(pszLinksTiles, "{x}") == nullptr || strstr(pszLinksTiles, "{y}") == nullptr ||
            strstr(pszLinksTiles, "{z}") == nullptr )
        {
            CPLError(CE_Warning, CPLE_NotSupported, "Invalid links.tiles = %s",
                     pszLinksTiles);
        }
        else
        {
            CPLString osCacheStr;
            if( !osCachePathRoot.empty() )
            {
                osCacheStr = "    <Cache><Path>";
                osCacheStr += GetMosaicCachePath();
                osCacheStr += "</Path><Unique>False</Unique></Cache>\n";
            }

            CPLString osTMSURL(pszLinksTiles);
            if( STARTS_WITH(pszLinksTiles, "https://") )
            {
                // Add API key as Basic auth
                osTMSURL = "https://";
                osTMSURL += osAPIKey;
                osTMSURL += ":@";
                osTMSURL += pszLinksTiles + strlen("https://");
            }
            ReplaceSubString(osTMSURL, "{x}", "${x}");
            ReplaceSubString(osTMSURL, "{y}", "${y}");
            ReplaceSubString(osTMSURL, "{z}", "${z}");
            ReplaceSubString(osTMSURL, "{0-3}", "0");

            CPLString osTMS = CPLSPrintf(
"<GDAL_WMS>\n"
"    <Service name=\"TMS\">\n"
"        <ServerUrl>%s</ServerUrl>\n"
"    </Service>\n"
"    <DataWindow>\n"
"        <UpperLeftX>%.16g</UpperLeftX>\n"
"        <UpperLeftY>%.16g</UpperLeftY>\n"
"        <LowerRightX>%.16g</LowerRightX>\n"
"        <LowerRightY>%.16g</LowerRightY>\n"
"        <TileLevel>%d</TileLevel>\n"
"        <TileCountX>1</TileCountX>\n"
"        <TileCountY>1</TileCountY>\n"
"        <YOrigin>top</YOrigin>\n"
"    </DataWindow>\n"
"    <Projection>%s</Projection>\n"
"    <BlockSizeX>256</BlockSizeX>\n"
"    <BlockSizeY>256</BlockSizeY>\n"
"    <BandsCount>4</BandsCount>\n"
"%s"
"</GDAL_WMS>",
                osTMSURL.c_str(),
                adfGeoTransform[0],
                adfGeoTransform[3],
                adfGeoTransform[0] + nRasterXSize * adfGeoTransform[1],
                adfGeoTransform[3] + nRasterYSize * adfGeoTransform[5],
                nZoomLevel,
                pszSRS,
                osCacheStr.c_str());
            //CPLDebug("PLMosaic", "TMS : %s", osTMS.c_str());

            poTMSDS = reinterpret_cast<GDALDataset *>(
                GDALOpenEx( osTMS, GDAL_OF_RASTER | GDAL_OF_INTERNAL,
                           nullptr, nullptr, nullptr ) );
        }
    }

    if( bUseTMSForMain && poTMSDS == nullptr )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Cannot find tile definition, so use_tiles will be ignored");
        bUseTMSForMain = FALSE;
    }

    for(int i=0;i<4;i++)
        SetBand(i + 1, new PLMosaicRasterBand(this, i + 1, eDT));

    json_object* poFirstAcquired = CPL_json_object_object_get(poObj, "first_acquired");
    if( poFirstAcquired != nullptr && json_object_get_type(poFirstAcquired) == json_type_string )
    {
        SetMetadataItem("FIRST_ACQUIRED",
                                 json_object_get_string(poFirstAcquired));
    }
    json_object* poLastAcquired = CPL_json_object_object_get(poObj, "last_acquired");
    if( poLastAcquired != nullptr && json_object_get_type(poLastAcquired) == json_type_string )
    {
        SetMetadataItem("LAST_ACQUIRED",
                                 json_object_get_string(poLastAcquired));
    }
    json_object* poTitle = CPL_json_object_object_get(poObj, "title");
    if( poTitle != nullptr && json_object_get_type(poTitle) == json_type_string )
    {
        SetMetadataItem("TITLE", json_object_get_string(poTitle));
    }

    json_object_put(poObj);
    return TRUE;
}

/************************************************************************/
/*                          ListSubdatasets()                           */
/************************************************************************/

int PLMosaicDataset::ListSubdatasets()
{
    CPLString osURL(osBaseURL);
    CPLStringList aosSubdatasets;
    while(osURL.size())
    {
        json_object* poObj = RunRequest(osURL);
        if( poObj == nullptr )
        {
            return FALSE;
        }

        osURL = "";
        json_object* poLinks = CPL_json_object_object_get(poObj, "links");
        if( poLinks != nullptr && json_object_get_type(poLinks) == json_type_object )
        {
            json_object* poNext = CPL_json_object_object_get(poLinks, "next");
            if( poNext != nullptr && json_object_get_type(poNext) == json_type_string )
            {
                osURL = json_object_get_string(poNext);
            }
        }

        json_object* poMosaics = CPL_json_object_object_get(poObj, "mosaics");
        if( poMosaics == nullptr || json_object_get_type(poMosaics) != json_type_array )
        {
            json_object_put(poObj);
            return FALSE;
        }

        const int nMosaics = json_object_array_length(poMosaics);
        for(int i=0;i< nMosaics;i++)
        {
            const char* pszName = nullptr;
            const char* pszTitle = nullptr;
            const char* pszSelf = nullptr;
            const char* pszCoordinateSystem = nullptr;
            json_object* poMosaic = json_object_array_get_idx(poMosaics, i);
            if( poMosaic && json_object_get_type(poMosaic) == json_type_object )
            {
                json_object* poName = CPL_json_object_object_get(poMosaic, "name");
                if( poName != nullptr && json_object_get_type(poName) == json_type_string )
                {
                    pszName = json_object_get_string(poName);
                }
                json_object* poTitle = CPL_json_object_object_get(poMosaic, "title");
                if( poTitle != nullptr && json_object_get_type(poTitle) == json_type_string )
                {
                    pszTitle = json_object_get_string(poTitle);
                }
                poLinks = CPL_json_object_object_get(poMosaic, "links");
                if( poLinks != nullptr && json_object_get_type(poLinks) == json_type_object )
                {
                    json_object* poSelf = CPL_json_object_object_get(poLinks, "self");
                    if( poSelf != nullptr && json_object_get_type(poSelf) == json_type_string )
                    {
                        pszSelf = json_object_get_string(poSelf);
                    }
                }
                json_object* poCoordinateSystem = CPL_json_object_object_get(poMosaic, "coordinate_system");
                if( poCoordinateSystem && json_object_get_type(poCoordinateSystem) == json_type_string )
                {
                    pszCoordinateSystem = json_object_get_string(poCoordinateSystem);
                }
            }

            if( pszName && pszSelf && pszCoordinateSystem &&
                EQUAL(pszCoordinateSystem, "EPSG:3857") )
            {
                const int nDatasetIdx = aosSubdatasets.Count() / 2 + 1;
                aosSubdatasets.AddNameValue(
                    CPLSPrintf("SUBDATASET_%d_NAME", nDatasetIdx),
                    CPLSPrintf("PLMOSAIC:mosaic=%s", pszName));
                if( pszTitle )
                {
                    aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_DESC", nDatasetIdx),
                        pszTitle);
                }
                else
                {
                    aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_DESC", nDatasetIdx),
                        CPLSPrintf("Mosaic %s", pszName));
                }
            }
        }

        json_object_put(poObj);
    }
    SetMetadata(aosSubdatasets.List(), "SUBDATASETS");
    return TRUE;
}

/************************************************************************/
/*                            GetProjectionRef()                       */
/************************************************************************/

const char* PLMosaicDataset::GetProjectionRef()
{
    return (pszWKT) ? pszWKT : "";
}

/************************************************************************/
/*                            GetGeoTransform()                         */
/************************************************************************/

CPLErr PLMosaicDataset::GetGeoTransform(double* padfGeoTransform)
{
    memcpy(padfGeoTransform, adfGeoTransform, 6 * sizeof(double));
    return ( bHasGeoTransform ) ? CE_None : CE_Failure;
}

/************************************************************************/
/*                          formatTileName()                            */
/************************************************************************/

CPLString PLMosaicDataset::formatTileName(int tile_x, int tile_y)

{
    CPLString result = osQuadPattern;

    size_t nPos = osQuadPattern.find("{tilex:");
    nPos += strlen("{tilex:");
    int nDigits;
    if( sscanf(osQuadPattern.c_str() + nPos, "0%dd}", &nDigits) != 1 ||
        nDigits <= 0 || nDigits > 9 )
        return result;
    CPLString fragment;
    fragment.Printf(CPLSPrintf("%%0%dd", nDigits), tile_x);
    ReplaceSubString(result, CPLSPrintf("{tilex:0%dd}", nDigits), fragment);

    nPos = osQuadPattern.find("{tiley:");
    nPos += strlen("{tiley:");
    if( sscanf(osQuadPattern.c_str() + nPos, "0%dd}", &nDigits) != 1 ||
        nDigits <= 0 || nDigits > 9 )
        return result;
    fragment.Printf(CPLSPrintf("%%0%dd", nDigits), tile_y);
    ReplaceSubString(result, CPLSPrintf("{tiley:0%dd}", nDigits), fragment);

    fragment.Printf("%d", nZoomLevel);
    ReplaceSubString(result, "{glevel:d}", fragment);

    return result;
}

/************************************************************************/
/*                          InsertNewDataset()                          */
/************************************************************************/

void PLMosaicDataset::InsertNewDataset(CPLString osKey, GDALDataset* poDS)
{
    if( static_cast<int>( oMapLinkedDatasets.size() ) == nCacheMaxSize )
    {
        CPLDebug("PLMOSAIC", "Discarding older entry %s from cache",
                 psTail->osKey.c_str());
        oMapLinkedDatasets.erase(psTail->osKey);
        PLLinkedDataset* psNewTail = psTail->psPrev;
        psNewTail->psNext = nullptr;
        if( psTail->poDS )
            GDALClose( psTail->poDS );
        delete psTail;
        psTail = psNewTail;
    }

    PLLinkedDataset* psLinkedDataset = new PLLinkedDataset();
    if( psHead )
        psHead->psPrev = psLinkedDataset;
    psLinkedDataset->osKey = osKey;
    psLinkedDataset->psNext = psHead;
    psLinkedDataset->poDS = poDS;
    psHead = psLinkedDataset;
    if( psTail == nullptr )
        psTail = psHead;
    oMapLinkedDatasets[osKey] = psLinkedDataset;
}

/************************************************************************/
/*                         OpenAndInsertNewDataset()                    */
/************************************************************************/

GDALDataset* PLMosaicDataset::OpenAndInsertNewDataset(CPLString osTmpFilename,
                                                      CPLString osTilename)
{
    const char* const apszAllowedDrivers[2] = { "GTiff", nullptr };
    GDALDataset* poDS = reinterpret_cast<GDALDataset *>(
        GDALOpenEx( osTmpFilename, GDAL_OF_RASTER | GDAL_OF_INTERNAL,
                    apszAllowedDrivers, nullptr, nullptr ) );
    if( poDS != nullptr )
    {
        if( poDS->GetRasterXSize() != nQuadSize ||
            poDS->GetRasterYSize() != nQuadSize ||
            poDS->GetRasterCount() != 4 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent metatile characteristics");
            GDALClose(poDS);
            poDS = nullptr;
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid GTiff dataset: %s",
                 osTilename.c_str());
    }

    InsertNewDataset(osTilename, poDS);
    return poDS;
}

/************************************************************************/
/*                            GetMetaTile()                             */
/************************************************************************/

GDALDataset* PLMosaicDataset::GetMetaTile(int tile_x, int tile_y)
{
    const CPLString osTilename = formatTileName(tile_x, tile_y);
    std::map<CPLString,PLLinkedDataset*>::const_iterator it =
                                                    oMapLinkedDatasets.find(osTilename);
    if( it == oMapLinkedDatasets.end() )
    {
        CPLString osTmpFilename;

        const CPLString osMosaicPath(GetMosaicCachePath());
        osTmpFilename = CPLFormFilename(osMosaicPath,
                CPLSPrintf("%s_%s.tif", osMosaic.c_str(), CPLGetFilename(osTilename)), nullptr);
        VSIStatBufL sStatBuf;
        if( !osCachePathRoot.empty() && VSIStatL(osTmpFilename, &sStatBuf) == 0 )
        {
            if( bTrustCache )
            {
                return OpenAndInsertNewDataset(osTmpFilename, osTilename);
            }

            CPLDebug("PLMOSAIC", "File %s exists. Checking if it is up-to-date...",
                     osTmpFilename.c_str());
            // Fetch metatile metadata
            json_object* poObj = RunRequest((osQuadsURL + osTilename).c_str());
            if( poObj == nullptr )
            {
                CPLDebug("PLMOSAIC", "Cannot get tile metadata");
                InsertNewDataset(osTilename, nullptr);
                return nullptr;
            }

            // Currently we only check by file size, which should be good enough
            // as the metatiles are compressed, so a change in content is likely
            // to cause a change in filesize. Use of a signature would be better
            // though if available in the metadata
            int nFileSize = 0;
            json_object* poProperties = CPL_json_object_object_get(poObj, "properties");
            if( poProperties && json_object_get_type(poProperties) == json_type_object )
            {
                json_object* poFileSize = CPL_json_object_object_get(poProperties, "file_size");
                nFileSize = json_object_get_int(poFileSize);
            }
            json_object_put(poObj);
            if( static_cast<int>( sStatBuf.st_size ) == nFileSize )
            {
                CPLDebug("PLMOSAIC", "Cached tile is up-to-date");
                return OpenAndInsertNewDataset(osTmpFilename, osTilename);
            }
            else
            {
                CPLDebug("PLMOSAIC", "Cached tile is not up-to-date");
                VSIUnlink(osTmpFilename);
            }
        }

        // Fetch the GeoTIFF now
        CPLString osURL = osQuadsURL;
        osURL += osTilename;
        osURL += "/full";

        CPLHTTPResult* psResult = Download(osURL, TRUE);
        if( psResult == nullptr )
        {
            InsertNewDataset(osTilename, nullptr);
            return nullptr;
        }

        CreateMosaicCachePathIfNecessary();

        VSILFILE* fp = osCachePathRoot.size() ? VSIFOpenL(osTmpFilename, "wb") : nullptr;
        if( fp )
        {
            VSIFWriteL(psResult->pabyData, 1, psResult->nDataLen, fp);
            VSIFCloseL(fp);
        }
        else
        {
            // In case there's no temporary path or it is not writable
            // use a in-memory dataset, and limit the cache to only one
            if( !osCachePathRoot.empty() && nCacheMaxSize > 1 )
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot write into %s. Using /vsimem and reduce cache to 1 entry",
                         osCachePathRoot.c_str());
                FlushDatasetsCache();
                nCacheMaxSize = 1;
            }
            osTmpFilename =
                CPLSPrintf("/vsimem/single_tile_plmosaic_cache/%s/%d_%d.tif",
                           osMosaic.c_str(), tile_x, tile_y);
            fp = VSIFOpenL(osTmpFilename, "wb");
            if( fp )
            {
                VSIFWriteL(psResult->pabyData, 1, psResult->nDataLen, fp);
                VSIFCloseL(fp);
            }
        }
        CPLHTTPDestroyResult(psResult);
        GDALDataset* poDS = OpenAndInsertNewDataset(osTmpFilename, osTilename);

        if( STARTS_WITH(osTmpFilename, "/vsimem/single_tile_plmosaic_cache/") )
            VSIUnlink(osTilename);

        return poDS;
    }

    // Move link to head of MRU list
    PLLinkedDataset* psLinkedDataset = it->second;
    GDALDataset* poDS = psLinkedDataset->poDS;
    if( psLinkedDataset != psHead )
    {
        if( psLinkedDataset == psTail )
            psTail = psLinkedDataset->psPrev;
        if( psLinkedDataset->psPrev )
            psLinkedDataset->psPrev->psNext = psLinkedDataset->psNext;
        if( psLinkedDataset->psNext )
            psLinkedDataset->psNext->psPrev = psLinkedDataset->psPrev;
        psLinkedDataset->psNext = psHead;
        psLinkedDataset->psPrev = nullptr;
        psHead->psPrev = psLinkedDataset;
        psHead = psLinkedDataset;
    }

    return poDS;
}

/************************************************************************/
/*                       GetLocationInfo()                          */
/************************************************************************/

const char* PLMosaicDataset::GetLocationInfo(int nPixel, int nLine)
{
    int nBlockXSize, nBlockYSize;
    GetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);

    const int nBlockXOff = nPixel / nBlockXSize;
    const int nBlockYOff = nLine / nBlockYSize;
    const int bottom_yblock = (nRasterYSize - nBlockYOff * nBlockYSize) / nBlockYSize - 1;

    const int meta_tile_x = (nBlockXOff * nBlockXSize) / nQuadSize;
    const int meta_tile_y = (bottom_yblock * nBlockYSize) / nQuadSize;

    CPLString osQuadURL = osQuadsURL;
    CPLString osTilename = formatTileName(meta_tile_x, meta_tile_y);
    osQuadURL += osTilename;

    if( meta_tile_x != nLastMetaTileX || meta_tile_y != nLastMetaTileY )
    {
        CPLHTTPResult* psResult = Download(osQuadURL, TRUE);
        if( psResult )
            osLastQuadInformation = (const char*) psResult->pabyData;
        else
            osLastQuadInformation = "";
        CPLHTTPDestroyResult(psResult);

        const CPLString osQuadScenesURL = osQuadURL + "/scenes/";

        psResult = Download(osQuadScenesURL, TRUE);
        if( psResult )
            osLastQuadSceneInformation = (const char*) psResult->pabyData;
        else
            osLastQuadSceneInformation = "";
        CPLHTTPDestroyResult(psResult);

        nLastMetaTileX = meta_tile_x;
        nLastMetaTileY = meta_tile_y;
    }

    osLastRetGetLocationInfo = "";

    CPLXMLNode* psRoot = CPLCreateXMLNode(nullptr, CXT_Element, "LocationInfo");

    if( !osLastQuadInformation.empty() )
    {
        const char* const apszAllowedDrivers[2] = { "GeoJSON", nullptr };
        const char* const apszOptions[2] = { "FLATTEN_NESTED_ATTRIBUTES=YES", nullptr };
        CPLString osTmpJSonFilename;
        osTmpJSonFilename.Printf("/vsimem/plmosaic/%p/quad.json", this);

        VSIFCloseL(VSIFileFromMemBuffer(
            osTmpJSonFilename,
            reinterpret_cast<GByte *>( const_cast<char *>(
                osLastQuadInformation.c_str() ) ),
            osLastQuadInformation.size(), FALSE));

        GDALDataset* poDS = reinterpret_cast<GDALDataset *>(
            GDALOpenEx( osTmpJSonFilename, GDAL_OF_VECTOR,
                        apszAllowedDrivers, apszOptions, nullptr ) );
        VSIUnlink(osTmpJSonFilename);

        if( poDS )
        {
            CPLXMLNode* psQuad = CPLCreateXMLNode(psRoot, CXT_Element, "Quad");
            OGRLayer* poLayer = poDS->GetLayer(0);
            OGRFeature* poFeat = nullptr;
            while( (poFeat = poLayer->GetNextFeature()) != nullptr )
            {
                for(int i=0;i<poFeat->GetFieldCount();i++)
                {
                    if( poFeat->IsFieldSetAndNotNull(i) )
                    {
                        CPLXMLNode* psItem = CPLCreateXMLNode(psQuad,
                            CXT_Element, poFeat->GetFieldDefnRef(i)->GetNameRef());
                        CPLCreateXMLNode(psItem, CXT_Text, poFeat->GetFieldAsString(i));
                    }
                }
                OGRGeometry* poGeom = poFeat->GetGeometryRef();
                if( poGeom )
                {
                    CPLXMLNode* psItem = CPLCreateXMLNode(psQuad, CXT_Element, "geometry");
                    char* l_pszWKT = nullptr;
                    poGeom->exportToWkt(&l_pszWKT);
                    CPLCreateXMLNode(psItem, CXT_Text, l_pszWKT);
                    CPLFree(l_pszWKT);
                }
                delete poFeat;
            }

            GDALClose(poDS);
        }
    }

    if( !osLastQuadSceneInformation.empty() && pszWKT != nullptr )
    {
        const char* const apszAllowedDrivers[2] = { "GeoJSON", nullptr };
        const char* const apszOptions[2] = { "FLATTEN_NESTED_ATTRIBUTES=YES", nullptr };
        CPLString osTmpJSonFilename;
        osTmpJSonFilename.Printf("/vsimem/plmosaic/%p/scenes.json", this);

        VSIFCloseL(VSIFileFromMemBuffer(
            osTmpJSonFilename,
            reinterpret_cast<GByte *>( const_cast<char *>(
                osLastQuadSceneInformation.c_str() ) ),
            osLastQuadSceneInformation.size(), FALSE));

        GDALDataset* poDS = reinterpret_cast<GDALDataset*>(
            GDALOpenEx( osTmpJSonFilename, GDAL_OF_VECTOR,
                        apszAllowedDrivers, apszOptions, nullptr ) );
        VSIUnlink(osTmpJSonFilename);

        OGRSpatialReference oSRSSrc, oSRSDst;
        oSRSSrc.SetFromUserInput(pszWKT);
        oSRSDst.SetFromUserInput(SRS_WKT_WGS84);
        OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(&oSRSSrc,
                                                                              &oSRSDst);
        double x = adfGeoTransform[0] + nPixel * adfGeoTransform[1];
        double y = adfGeoTransform[3] + nLine * adfGeoTransform[5];
        if( poDS && poCT && poCT->Transform(1, &x, &y))
        {
            CPLXMLNode* psScenes = nullptr;
            OGRLayer* poLayer = poDS->GetLayer(0);
            poLayer->SetSpatialFilterRect(x,y,x,y);
            OGRFeature* poFeat = nullptr;
            while( (poFeat = poLayer->GetNextFeature()) != nullptr )
            {
                OGRGeometry* poGeom = poFeat->GetGeometryRef();
                if( poGeom )
                {
                    if( psScenes == nullptr )
                        psScenes = CPLCreateXMLNode(psRoot, CXT_Element, "Scenes");
                    CPLXMLNode* psScene = CPLCreateXMLNode(psScenes, CXT_Element, "Scene");
                    for(int i=0;i<poFeat->GetFieldCount();i++)
                    {
                        if( poFeat->IsFieldSetAndNotNull(i) )
                        {
                            CPLXMLNode* psItem = CPLCreateXMLNode(psScene,
                                CXT_Element, poFeat->GetFieldDefnRef(i)->GetNameRef());
                            CPLCreateXMLNode(psItem, CXT_Text, poFeat->GetFieldAsString(i));
                        }
                    }
                    CPLXMLNode* psItem = CPLCreateXMLNode(psScene, CXT_Element, "geometry");
                    char* l_pszWKT = nullptr;
                    poGeom->exportToWkt(&l_pszWKT);
                    CPLCreateXMLNode(psItem, CXT_Text, l_pszWKT);
                    CPLFree(l_pszWKT);
                }
                delete poFeat;
            }
        }
        delete poCT;
        if( poDS )
            GDALClose(poDS);
    }

    char* pszXML = CPLSerializeXMLTree(psRoot);
    CPLDestroyXMLNode(psRoot);
    osLastRetGetLocationInfo = pszXML;
    CPLFree(pszXML);

    return osLastRetGetLocationInfo.c_str();
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr  PLMosaicDataset::IRasterIO( GDALRWFlag eRWFlag,
                               int nXOff, int nYOff, int nXSize, int nYSize,
                               void * pData, int nBufXSize, int nBufYSize,
                               GDALDataType eBufType,
                               int nBandCount, int *panBandMap,
                               GSpacing nPixelSpace, GSpacing nLineSpace,
                               GSpacing nBandSpace,
                               GDALRasterIOExtraArg* psExtraArg)
{
    if( bUseTMSForMain && poTMSDS )
        return poTMSDS->RasterIO( eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                  pData, nBufXSize, nBufYSize,
                                  eBufType, nBandCount, panBandMap,
                                  nPixelSpace, nLineSpace, nBandSpace,
                                  psExtraArg );

    return BlockBasedRasterIO( eRWFlag, nXOff, nYOff, nXSize, nYSize,
                               pData, nBufXSize, nBufYSize,
                               eBufType, nBandCount, panBandMap,
                               nPixelSpace, nLineSpace, nBandSpace,
                               psExtraArg );
}

/************************************************************************/
/*                      GDALRegister_PLMOSAIC()                         */
/************************************************************************/

void GDALRegister_PLMOSAIC()

{
    if( GDALGetDriverByName( "PLMOSAIC" ) != nullptr )
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription( "PLMOSAIC" );
    poDriver->SetMetadataItem( GDAL_DCAP_RASTER, "YES" );
    poDriver->SetMetadataItem( GDAL_DMD_LONGNAME,
                               "Planet Labs Mosaics API" );
    poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC,
                               "frmt_plmosaic.html" );

    poDriver->SetMetadataItem( GDAL_DMD_CONNECTION_PREFIX, "PLMOSAIC:" );

    poDriver->SetMetadataItem( GDAL_DMD_OPENOPTIONLIST,
"<OpenOptionList>"
"  <Option name='API_KEY' type='string' description='Account API key' required='true'/>"
"  <Option name='MOSAIC' type='string' description='Mosaic name'/>"
"  <Option name='CACHE_PATH' type='string' description='Directory where to put cached quads'/>"
"  <Option name='TRUST_CACHE' type='boolean' description='Whether already cached quads should be trusted as the most recent version' default='NO'/>"
"  <Option name='USE_TILES' type='boolean' description='Whether to use the tile API even for full resolution data (only for Byte mosaics)' default='NO'/>"
"</OpenOptionList>" );

    poDriver->pfnIdentify = PLMosaicDataset::Identify;
    poDriver->pfnOpen = PLMosaicDataset::Open;

    GetGDALDriverManager()->RegisterDriver( poDriver );
}
