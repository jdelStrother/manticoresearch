//
// Copyright (c) 2017-2019, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxint.h"
#include "icu.h"

#if USE_ICU

#ifndef U_STATIC_IMPLEMENTATION
#define U_STATIC_IMPLEMENTATION
#endif


#ifndef U_CHARSET_IS_UTF8
#define U_CHARSET_IS_UTF8 1
#endif

#ifndef U_NO_DEFAULT_INCLUDE_UTF_HEADERS
#define U_NO_DEFAULT_INCLUDE_UTF_HEADERS 1
#endif


#include <unicode/brkiter.h>
#include <unicode/udata.h>
#include <unicode/ustring.h>

#ifndef ICU_LIB
#define ICU_LIB "no_icu_configured"
#endif

#if DL_ICU
#include "ldicu.inl"
#else
#define imp_createWordInstance icu::BreakIterator::createWordInstance
#define imp_u_errorName u_errorName
#define imp_u_setDataDirectory u_setDataDirectory
#define imp_utext_openUTF8 utext_openUTF8
#define imp_utext_close utext_close
#define imp_getChinese icu::Locale::getChinese
#define InitDynamicIcu() (true)
#endif

class ICUPreprocessor_c
{
public:
							~ICUPreprocessor_c();

	bool					Init ( CSphString & sError );
	bool					Process ( const BYTE * pBuffer, int iLength, CSphVector<BYTE> & dOut, bool bQuery );
	bool					SetBlendChars ( const char * szBlendChars, CSphString & sError );

protected:
	CSphString				m_sBlendChars;

private:
	icu::BreakIterator *	m_pBreakIterator {nullptr};
	const BYTE *			m_pBuffer {nullptr};
	int						m_iBoundaryIndex {0};
	int						m_iPrevBoundary {0};

	CSphVector<CSphRemapRange> m_dBlendChars;


	void					AddTextChunk ( const BYTE * pStart, int iLen, CSphVector<BYTE> & dOut, bool bChinese, bool bQuery );
	bool					NeedAddSpace ( const BYTE * pToken, const CSphVector<BYTE> & dOut, bool bQuery ) const;

	void					ProcessBufferICU ( const BYTE * pBuffer, int iLength );
	const BYTE *			GetNextTokenICU ( int & iTokenLen );

	bool					IsChineseSeparator ( int iCode ) const;
	bool					IsSpecialQueryCode ( int iCode ) const;
	bool					IsBlendChar ( int iCode ) const;
};


ICUPreprocessor_c::~ICUPreprocessor_c()
{
	SafeDelete(m_pBreakIterator);
}


bool ICUPreprocessor_c::Init ( CSphString & sError )
{
	assert ( !m_pBreakIterator );

	if_const ( !InitDynamicIcu ())
	{
		sError.SetSprintf ( "ICU: failed to load icu library (tried "  ICU_LIB ")" );
		return false;
	}

	UErrorCode tStatus = U_ZERO_ERROR;
	m_pBreakIterator = imp_createWordInstance ( imp_getChinese(), tStatus );
	if ( U_FAILURE(tStatus) )
	{
		sError.SetSprintf( "Unable to initialize ICU break iterator: %s", imp_u_errorName(tStatus) );
		if ( tStatus==U_MISSING_RESOURCE_ERROR )
			sError.SetSprintf ( "%s. Make sure ICU data file is accessible (icu_data_dir might be missing in config)", sError.cstr() );

		return false;			
	}

	if ( !m_pBreakIterator )
	{
		sError = "Unable to initialize ICU break iterator";
		return false;
	}

	return true;
}


bool ICUPreprocessor_c::Process ( const BYTE * pBuffer, int iLength, CSphVector<BYTE> & dOut, bool bQuery )
{
	if ( !pBuffer || !iLength )
		return false;

	if ( !sphDetectChinese ( pBuffer, iLength ) )
		return false;

	dOut.Resize(0);

	const BYTE * pBufferMax = pBuffer+iLength;

	bool bWasChineseCode = false;
	const BYTE * pChunkStart = pBuffer;
	bool bFirstCode = true;
	while ( pBuffer<pBufferMax )
	{
		const BYTE * pTmp = pBuffer;
		int iCode = sphUTF8Decode ( pBuffer );
		bool bIsChineseCode = sphIsChineseCode(iCode);
		if ( !bFirstCode && bWasChineseCode!=bIsChineseCode )
		{
			AddTextChunk ( pChunkStart, pTmp-pChunkStart, dOut, bWasChineseCode, bQuery );
			pChunkStart = pTmp;
		}

		bWasChineseCode = bIsChineseCode;
		bFirstCode = false;
	}

	AddTextChunk ( pChunkStart, pBuffer-pChunkStart, dOut, bWasChineseCode, bQuery );

	return true;
}


bool ICUPreprocessor_c::SetBlendChars ( const char * szBlendChars, CSphString & sError )
{
	m_sBlendChars = szBlendChars;
	CSphCharsetDefinitionParser tParser;
	if ( !tParser.Parse ( szBlendChars, m_dBlendChars ) )
	{
		sError = tParser.GetLastError();
		return false;
	}

	return true;
}


bool ICUPreprocessor_c::NeedAddSpace ( const BYTE * pToken, const CSphVector<BYTE> & dOut, bool bQuery ) const
{
	int iResLen = dOut.GetLength();
	if ( !iResLen )
		return false;

	if ( bQuery && ( IsSpecialQueryCode ( dOut[iResLen - 1] ) || IsSpecialQueryCode ( *pToken ) ) )
		return false;

	if ( IsBlendChar( dOut[iResLen - 1] ) || IsBlendChar ( *pToken ) )
		return false;

	return !sphIsSpace ( dOut[iResLen-1] ) && !sphIsSpace ( *pToken );
}


void ICUPreprocessor_c::AddTextChunk ( const BYTE * pStart, int iLen, CSphVector<BYTE> & dOut, bool bChinese, bool bQuery )
{
	if ( !iLen )
		return;

	if ( bChinese )
	{
		ProcessBufferICU ( pStart, iLen );

		const BYTE * pToken;
		int iTokenLen = 0;
		while ( (pToken = GetNextTokenICU(iTokenLen))!=nullptr )
		{
			bool bAddSpace = NeedAddSpace ( pToken, dOut, bQuery );

			BYTE * pOut = dOut.AddN ( iTokenLen + ( bAddSpace ? 1 : 0 ) );
			if ( bAddSpace )
				*pOut++ = ' ';

			memcpy ( pOut, pToken, iTokenLen );
		}
	}
	else
	{
		bool bAddSpace = NeedAddSpace ( pStart, dOut, bQuery );
		BYTE * pOut = dOut.AddN ( iLen + ( bAddSpace ? 1 : 0 ) );
		if ( bAddSpace )
			*pOut++ = ' ';

		memcpy ( pOut, pStart, iLen );
	}
}


void ICUPreprocessor_c::ProcessBufferICU ( const BYTE * pBuffer, int iLength )
{
	assert ( m_pBreakIterator );
	UErrorCode tStatus = U_ZERO_ERROR;
	UText * pUText = nullptr;
	pUText = imp_utext_openUTF8 ( pUText, (const char*)pBuffer, iLength, &tStatus );
	if ( U_FAILURE(tStatus) )
		sphWarning ( "Error processing buffer (ICU): %s", imp_u_errorName(tStatus) );

	assert ( pUText );
	m_pBreakIterator->setText ( pUText, tStatus );
	if ( U_FAILURE(tStatus) )
		sphWarning ( "Error processing buffer (ICU): %s", imp_u_errorName(tStatus) );

	imp_utext_close ( pUText );

	m_pBuffer = pBuffer;
	m_iPrevBoundary = m_iBoundaryIndex = m_pBreakIterator->first();
}


const BYTE * ICUPreprocessor_c::GetNextTokenICU ( int & iTokenLen )
{
	if ( !m_pBreakIterator || m_iBoundaryIndex==icu::BreakIterator::DONE )
		return nullptr;

	while ( ( m_iBoundaryIndex = m_pBreakIterator->next() )!=icu::BreakIterator::DONE )
	{
		int iLength = m_iBoundaryIndex-m_iPrevBoundary;

		// ltrim
		const BYTE * pStart = m_pBuffer+m_iPrevBoundary;
		const BYTE * pMax = pStart + iLength;
		while ( pStart<pMax && sphIsSpace(*pStart) )
			pStart++;

		// rtrim
		while ( pStart<pMax && sphIsSpace(*(pMax-1)) )
			pMax--;

		m_iPrevBoundary = m_iBoundaryIndex;

		if ( pStart!=pMax )
		{
			iTokenLen = pMax-pStart;
			return pStart;
		}
	}

	return nullptr;
}


bool ICUPreprocessor_c::IsChineseSeparator ( int iCode ) const
{
	return ( iCode>=0x3000 && iCode<=0x303F ) || sphIsSpace(iCode);
}


bool ICUPreprocessor_c::IsSpecialQueryCode ( int iCode ) const
{
	return iCode=='!' || iCode=='^' || iCode=='$' || iCode=='*' || iCode=='=';
}


bool ICUPreprocessor_c::IsBlendChar ( int iCode ) const
{
	ARRAY_FOREACH ( i, m_dBlendChars )
		if ( iCode>=m_dBlendChars[i].m_iStart && iCode<=m_dBlendChars[i].m_iEnd )
			return true;

	return false;
}


//////////////////////////////////////////////////////////////////////////
class FieldFilterICU_c : public ISphFieldFilter, public ICUPreprocessor_c
{
public:
	int				Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage, bool bQuery ) final;
	void			GetSettings ( CSphFieldFilterSettings & tSettings ) const final;
	ISphFieldFilter * Clone() final;

protected:
					~FieldFilterICU_c() {}
};


int FieldFilterICU_c::Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage, bool bQuery )
{
	if ( m_pParent )
	{
		int iResultLength = m_pParent->Apply ( sField, iLength, dStorage, bQuery );
		if ( iResultLength ) // can't use dStorage.GetLength() because of the safety gap
		{
			CSphFixedVector<BYTE> dTmp ( iResultLength );
			memcpy ( dTmp.Begin(), dStorage.Begin(), dStorage.GetLength() );
			if ( !Process ( dTmp.Begin(), iLength, dStorage, bQuery ) )
				return iResultLength;

			// add safety gap
			int iStorageLength = dStorage.GetLength();
			if ( iStorageLength )
			{
				dStorage.Resize ( iStorageLength+4 );
				dStorage[iStorageLength]='\0';
			}

			return iStorageLength;
		}
	}

	if ( !Process ( sField, iLength, dStorage, bQuery ) )
		return 0;

	int iStorageLength = dStorage.GetLength();
	*dStorage.AddN(4) = '\0';

	return iStorageLength;
}


void FieldFilterICU_c::GetSettings ( CSphFieldFilterSettings & tSettings ) const
{
	if ( m_pParent )
		m_pParent->GetSettings ( tSettings );
}


ISphFieldFilter * FieldFilterICU_c::Clone()
{
	ISphFieldFilterRefPtr_c pClonedParent { m_pParent ? m_pParent->Clone () : nullptr };

	CSphString sError;
	ISphFieldFilter * pFilter = sphCreateFilterICU ( pClonedParent, m_sBlendChars.cstr(), sError );
	if ( !pFilter )
		sphWarning ( "ICU filter clone error '%s'", sError.cstr() );

	return pFilter;
}

ISphFieldFilter * sphCreateFilterICU ( ISphFieldFilter * pParent, const char * szBlendChars, CSphString & sError )
{
	CSphRefcountedPtr<FieldFilterICU_c> pFilter { new FieldFilterICU_c };
	if ( !pFilter->Init ( sError ) )
	{
		SafeAddRef ( pParent )
		return pParent;
	}

	if ( szBlendChars && *szBlendChars && !pFilter->SetBlendChars ( szBlendChars, sError ) )
	{
		SafeAddRef ( pParent );
		return pParent;
	}

	pFilter->SetParent ( pParent );
	return pFilter.Leak ();
}


bool sphCheckConfigICU ( CSphIndexSettings &, CSphString & )
{
	return true;
}


bool sphSpawnFilterICU ( ISphFieldFilterRefPtr_c & pFieldFilter, const CSphIndexSettings & m_tSettings,	const CSphTokenizerSettings & tTokSettings, const char * szIndex, CSphString & sError )
{
	if ( m_tSettings.m_ePreprocessor==Preprocessor_e::NONE )
		return true;

	ISphFieldFilterRefPtr_c pFilterICU { sphCreateFilterICU ( pFieldFilter, tTokSettings.m_sBlendChars.cstr(), sError ) };
	if ( !sError.IsEmpty() )
	{
		sError.SetSprintf ( "index '%s': Error initializing ICU: %s", szIndex, sError.cstr() );
		return false;
	}

	pFieldFilter = pFilterICU;
	return true;
}


void sphConfigureICU ( CSphConfigSection & hCommon )
{
	if_const ( !InitDynamicIcu ())
	{
		sphWarning ( "ICU: failed to load icu library (tried "  ICU_LIB ")" );
		return;
	}

	CSphString sData = hCommon.GetStr ( "icu_data_dir" );
	imp_u_setDataDirectory ( sData.cstr() );
}

#else


ISphFieldFilter * sphCreateFilterICU ( ISphFieldFilter * pParent, CSphString & )
{
	SafeAddRef ( pParent );
	return pParent;
}


bool sphCheckConfigICU ( CSphIndexSettings & tSettings, CSphString & sError )
{
	if ( tSettings.m_ePreprocessor!=Preprocessor_e::NONE )
	{
		tSettings.m_ePreprocessor = Preprocessor_e::NONE;
		sError.SetSprintf ( "ICU options specified, but no ICU support compiled; ignoring\n" );
		return false;
	}

	return true;
}


bool sphSpawnFilterICU ( ISphFieldFilterRefPtr_c &, const CSphIndexSettings &, const CSphTokenizerSettings &, const char *, CSphString & )
{
	return true;
}

void sphConfigureICU ( CSphConfigSection & )
{
}

#endif