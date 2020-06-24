//(c) 2016-2020 by Authors
//This file is a part of Flye program.
//Released under the BSD license (see LICENSE file)

#include <chrono>
#include <iomanip>

#include "alignment.h"

#define HAVE_KALLOC
#include "kalloc.h"
#include "ksw2.h"
#undef HAVE_KALLOC

#include "edlib.h"

using namespace std::chrono;

namespace
{
	struct ThreadMemPool
	{
		ThreadMemPool():
			prevCleanup(system_clock::now() + seconds(rand() % 60))
		{
		   memPool = km_init();
		}
		~ThreadMemPool()
		{
		   km_destroy(memPool);
		}
		void cleanIter()
		{
			if ((system_clock::now() - prevCleanup) > seconds(60))
			{
				km_destroy(memPool);
				memPool = km_init();
				prevCleanup = system_clock::now();
			}
		}

		time_point<system_clock> prevCleanup;
		void* memPool;
	};

	struct CompressedSeq
	{
		DnaSequence seq;
		std::vector<int32_t> offsetTable;
	};

	CompressedSeq homopolymerCompression(const DnaSequence& seq, int32_t start, int32_t length,
										 bool doCompression)
	{
		std::string newSeq;
		newSeq.reserve(length);
		std::vector<int32_t> offsetTable;
		offsetTable.reserve(length);

		for (size_t i = 0; i < (size_t)length; ++i)
		{
			if (!doCompression || i == 0 || newSeq.back() != seq.at(i + start))
			{
				newSeq += seq.at(i + start);
				offsetTable.push_back(i);
			}
			
		}
		return {DnaSequence(newSeq), offsetTable};
	}

	/*void printAlignment(const std::string& alnQry, const std::string& alnTrg)
	{
		const int WIDTH = 100;
		std::stringstream ss;
		for (size_t chunk = 0; chunk <= alnQry.size() / WIDTH; ++chunk)
		{
			ss << std::setw(6) << chunk * WIDTH << " ";
			for (size_t i = chunk * WIDTH; 
				 i < std::min((chunk + 1) * WIDTH, alnQry.size()); ++i)
			{
				ss << alnQry[i];
			}
			ss << "\n       ";
			for (size_t i = chunk * WIDTH; 
				 i < std::min((chunk + 1) * WIDTH, alnQry.size()); ++i)
			{
				ss << (alnQry[i] == alnTrg[i] ? '|' : '*');
			}
			ss << "\n       ";
			for (size_t i = chunk * WIDTH; 
				 i < std::min((chunk + 1) * WIDTH, alnQry.size()); ++i)
			{
				ss << alnTrg[i];
			}
			ss << "\n\n";
		}
		Logger::get().debug() << "\n" << ss.str();
	}*/
}

float getAlignmentCigarKsw(const DnaSequence& trgSeq, size_t trgBegin, size_t trgLen,
			   			   const DnaSequence& qrySeq, size_t qryBegin, size_t qryLen,
			   			   int matchScore, int misScore, int gapOpen, int gapExtend,
			   			   float maxAlnErr, std::vector<CigOp>& cigarOut)
{
	thread_local ThreadMemPool buf;
	thread_local std::vector<uint8_t> trgByte;
	thread_local std::vector<uint8_t> qryByte;
	buf.cleanIter();
	trgByte.assign(trgLen, 0);
	qryByte.assign(qryLen, 0);

	for (size_t i = 0; i < trgLen; ++i)
	{
		trgByte[i] = trgSeq.atRaw(i + trgBegin);
	}
	for (size_t i = 0; i < qryLen; ++i)
	{
		qryByte[i] = qrySeq.atRaw(i + qryBegin);
	}

	//substitution matrix
	int8_t a = matchScore;
	int8_t b = misScore < 0 ? misScore : -misScore; // a > 0 and b < 0
	int8_t subsMat[] = {a, b, b, b, 0, 
						b, a, b, b, 0, 
						b, b, a, b, 0, 
						b, b, b, a, 0, 
						0, 0, 0, 0, 0};

	const int NUM_NUCL = 5;
	const int Z_DROP = -1;
	const int FLAG = KSW_EZ_APPROX_MAX | KSW_EZ_APPROX_DROP;
	const int END_BONUS = 0;
	
	//int seqDiff = abs((int)trgByte.size() - (int)qryByte.size());
	//int bandWidth = seqDiff + MAX_JUMP;
	//int bandWidth = std::max(10.0f, maxAlnErr * std::max(trgLen, qryLen));
	(void)maxAlnErr;

	//dynamic band selection
	ksw_extz_t ez;
	int bandWidth = 64;
	for (;;)
	{
		memset(&ez, 0, sizeof(ksw_extz_t));
		ksw_extz2_sse(buf.memPool, qryByte.size(), &qryByte[0], 
					  trgByte.size(), &trgByte[0], NUM_NUCL,
					  subsMat, gapOpen, gapExtend, bandWidth, Z_DROP, 
					  END_BONUS, FLAG, &ez);
		if (!ez.zdropped) break;
		if (bandWidth > (int)std::max(qryByte.size(), trgByte.size())) break; //just in case
		bandWidth *= 2;
	}
	//std::cout << bandWidth << std::endl;
	
	int numMatches = 0;
	int numMiss = 0;
	int numIndels = 0;

	cigarOut.clear();
	cigarOut.reserve((size_t)ez.n_cigar);

	//decode cigar
	size_t posQry = 0;
	size_t posTrg = 0;
	for (size_t i = 0; i < (size_t)ez.n_cigar; ++i)
	{
		int size = ez.cigar[i] >> 4;
		char op = "MID"[ez.cigar[i] & 0xf];
		//alnLength += size;

		if (op == 'M')
		{
			for (size_t i = 0; i < (size_t)size; ++i)
			{
				char match = "X="[size_t(trgByte[posTrg + i] == 
										 qryByte[posQry + i])];
				if (i == 0 || (match != cigarOut.back().op))
				{
					cigarOut.push_back({match, 1});
				}
				else
				{
					++cigarOut.back().len;
				}
				numMatches += int(match == '=');
				numMiss += int(match == 'X');
			}
			posQry += size;
			posTrg += size;
		}
		else if (op == 'I')
		{
			cigarOut.push_back({'I', size});
			posQry += size;
			numIndels += size;
		}
		else //D
		{
			cigarOut.push_back({'D', size});
			posTrg += size;
			numIndels += size;
		}
	}
	//float errRate = 1 - float(numMatches) / (numMatches + numMiss + numIndels);
	float errRate = float(numMiss + numIndels) / std::max(trgLen, qryLen);

	kfree(buf.memPool, ez.cigar);
	return errRate;
}

float getAlignmentErrEdlib(const OverlapRange& ovlp, const DnaSequence& trgSeq,
					  	   const DnaSequence& qrySeq, float maxAlnErr, bool useHpc)
{
	thread_local ThreadMemPool buf;
	buf.cleanIter();

	auto trgCompressed = homopolymerCompression(trgSeq, ovlp.curBegin, 
												ovlp.curRange(), useHpc);
	auto qryCompressed = homopolymerCompression(qrySeq, ovlp.extBegin, 
												ovlp.extRange(), useHpc);

	(void)maxAlnErr;
	//int bandWidth = std::max(10.0f, maxAlnErr * std::max(ovlp.curRange(), 
	//													 ovlp.extRange()));
	//letting edlib find k byt iterating over powers of 2. Seems like
	//it is in fact a little faster, than having a hard upper limit.
	auto edlibCfg = edlibNewAlignConfig(-1, EDLIB_MODE_NW, 
										EDLIB_TASK_DISTANCE, nullptr, 0);
	auto result = edlibAlign(&qryCompressed.seq.str().c_str()[0], qryCompressed.seq.length(),
							 &trgCompressed.seq.str().c_str()[0], trgCompressed.seq.length(), 
							 edlibCfg);
	//Logger::get().debug() << result.editDistance << " " << result.alignmentLength;
	if (result.editDistance < 0)
	{
		return 1.0f;
	}
	return (float)result.editDistance / std::max(qryCompressed.seq.length(), 
												 trgCompressed.seq.length());
	//return (float)result.editDistance / result.alignmentLength;
}


float getAlignmentErrKsw(const OverlapRange& ovlp,
					  	 const DnaSequence& trgSeq,
					  	 const DnaSequence& qrySeq,
					  	 float maxAlnErr)
{
	std::vector<CigOp> decodedCigar;
	float errRate = getAlignmentCigarKsw(trgSeq, ovlp.curBegin, ovlp.curRange(),
							 			 qrySeq, ovlp.extBegin, ovlp.extRange(),
							 			 /*match*/ 1, /*mm*/ -1, /*gap open*/ 1, 
							 			 /*gap ext*/ 1, maxAlnErr, decodedCigar);

	//visualize alignents if needed
	/*if (showAlignment)
	{
			}*/

	return errRate;
}


void decodeCigar(const std::vector<CigOp>& cigar, 
				 const DnaSequence& trgSeq, size_t trgBegin,
				 const DnaSequence& qrySeq, size_t qryBegin,
				 std::string& outAlnTrg, std::string& outAlnQry)
{
	outAlnTrg.clear();
	outAlnQry.clear();
	size_t posQry = 0;
	size_t posTrg = 0;
	for (auto& op : cigar)
	{
		if (op.op == '=' || op.op == 'X')
		{
			//alnQry += strQ.substr(posQry, op.len);
			//alnTrg += strT.substr(posTrg, op.len);
			outAlnQry += qrySeq.substr(qryBegin + posQry, op.len).str();
			outAlnTrg += trgSeq.substr(trgBegin + posTrg, op.len).str();
			posQry += op.len;
			posTrg += op.len;
		}
		else if (op.op == 'I')
		{
			outAlnQry += qrySeq.substr(qryBegin + posQry, op.len).str();
			outAlnTrg += std::string(op.len, '-');
			posQry += op.len;
		}
		else
		{
			outAlnQry += std::string(op.len, '-');
			outAlnTrg += trgSeq.substr(trgBegin + posTrg, op.len).str();
			posTrg += op.len;
		}
	}
}



std::vector<OverlapRange> 
	checkIdyAndTrim(OverlapRange& ovlp, const DnaSequence& curSeq,
					const DnaSequence& extSeq, float maxDivergence,
					int32_t minOverlap, bool useHpc)
{
	//homopolymer-compressed, if needed
	auto curCompressed = homopolymerCompression(curSeq, ovlp.curBegin, 
												ovlp.curRange(), useHpc);
	auto extCompressed = homopolymerCompression(extSeq, ovlp.extBegin, 
												ovlp.extRange(), useHpc);

	//recompute base alignment with cigar output
	std::vector<CigOp> cigar;
	float errRate = getAlignmentCigarKsw(curCompressed.seq, 0, curCompressed.seq.length(),
							 			 extCompressed.seq, 0, extCompressed.seq.length(),
							 			 /*match*/ 1, /*mm*/ -1, /*gap open*/ 1, 
							 			 /*gap ext*/ 1, maxDivergence, cigar);
	(void)errRate;

	/*if (errRate < maxDivergence) 	//should not normally happen
	{
		ovlp.seqDivergence = errRate;
		return {ovlp};
	}*/

	std::vector<int> sumErrors = {0};
	sumErrors.reserve(cigar.size() + 1);
	std::vector<int> sumCurLen = {0};
	sumCurLen.reserve(cigar.size() + 1);
	std::vector<int> sumExtLen = {0};
	sumExtLen.reserve(cigar.size() + 1);

	for (auto op : cigar)
	{
		int curConsumed = op.len;
		int extConsumed = op.len;
		if (op.op == 'I')
		{
			curConsumed = 0;
		}
		else if (op.op == 'D')
		{
			extConsumed = 0;
		}
		int errLen = (op.op != '=') ? op.len : 0;

		sumCurLen.push_back(sumCurLen.back() + curConsumed);
		sumExtLen.push_back(sumExtLen.back() + extConsumed);
		sumErrors.push_back(sumErrors.back() + errLen);
	}

	struct IntervalDiv
	{
		int start;
		int end;
		float divergence;
	};
	std::vector<IntervalDiv> goodIntervals;
	//const float EPS = 0.0001;

	for (int intLen = (int)cigar.size(); intLen > 0; --intLen)
	{
		for (int intStart = 0; 
			intStart < (int)cigar.size() - intLen + 1; ++intStart)
		{
			int i = intStart;
			int j = intStart + intLen - 1;
			//only consider intervals that end on matches
			if (cigar[i].op != '=' || cigar[j].op != '=') continue;

			int rangeLen = std::max(sumCurLen[j + 1] - sumCurLen[i],
									sumExtLen[j + 1] - sumExtLen[i]);
			int rangeErr = sumErrors[j + 1] - sumErrors[i];
			float divergence = float(rangeErr) / rangeLen;

			if (divergence < maxDivergence)
			{
				if (j - i >= 0) goodIntervals.push_back({i, j, divergence});
			}
		}
	}

	//select non-intersecting set
	std::vector<IntervalDiv> nonIntersecting;
	for (auto& interval : goodIntervals)
	{
		bool intersects = false;
		for (auto& otherInt : nonIntersecting)
		{
			int ovl = std::min(interval.end + 1, otherInt.end + 1) - 
					  std::max(interval.start, otherInt.start);
			if (ovl > 0) 
			{
				intersects = true;
				break;
			}
		}
		if (!intersects) nonIntersecting.push_back(interval);
	}

	//now, for each interesting interval check its length and create new overlaps
	//also need to transofrm back from HPC to original coordinates
	std::vector<OverlapRange> trimmedAlignments;
	for (auto intCand : nonIntersecting)
	{
		OverlapRange newOvlp = ovlp;
		newOvlp.seqDivergence = intCand.divergence;
		size_t posQry = 0;
		size_t posTrg = 0;
		for (int i = 0; i < (int)cigar.size(); ++i)
		{
			if (i == intCand.start)
			{
				newOvlp.curBegin += curCompressed.offsetTable[posTrg];
				newOvlp.extBegin += extCompressed.offsetTable[posQry];
			}

			if (cigar[i].op == '=' || cigar[i].op == 'X')
			{
				posQry += cigar[i].len;
				posTrg += cigar[i].len;
			}
			else if (cigar[i].op == 'I')
			{
				posQry += cigar[i].len;
			}
			else
			{
				posTrg += cigar[i].len;
			}

			if (i == intCand.end)
			{
				newOvlp.curEnd = ovlp.curBegin + curCompressed.offsetTable[posTrg - 1];
				newOvlp.extEnd = ovlp.extBegin + extCompressed.offsetTable[posQry - 1];
			}

		}
		//TODO: updating score and k-mer matches?
		
		if (newOvlp.curRange() > minOverlap &&
			newOvlp.extRange() > minOverlap)
		{
			trimmedAlignments.push_back(newOvlp);

		}	
	}

	//debugging block
	/*static std::mutex logLock;
	if (!trimmedAlignments.empty())
	{
		std::lock_guard<std::mutex> locko(logLock);

		std::vector<CigOp> cigar;
		std::string alnCur;
		std::string alnExt;
		float noHpcErr = 
			getAlignmentCigarKsw(curSeq, ovlp.curBegin, ovlp.curRange(),
								 extSeq, ovlp.extBegin, ovlp.extRange(),
								 1, -1, 1, 1, maxDivergence, cigar);
		decodeCigar(cigar, curSeq, ovlp.curBegin, extSeq, ovlp.extBegin,
					alnCur, alnExt);
		printAlignment(alnCur, alnExt);
		Logger::get().debug() << "Idy w/o hpc: " << noHpcErr << " with hpc: " << errRate;

		Logger::get().debug() << "Adj from cb:" << ovlp.curBegin 
			<< " cl:" << ovlp.curRange() << 
			" eb:" << ovlp.extBegin << " el:" << ovlp.extRange() 
			<< " cid:" << ovlp.curId << " eid:" << ovlp.extId
			<< " err:" << errRate;

		for (auto& newOvlp : trimmedAlignments)
		{
			Logger::get().debug() << "      to cb:" << newOvlp.curBegin 
				<< " cl:" << newOvlp.curRange() << 
				" eb:" << newOvlp.extBegin << " el:" << newOvlp.extRange() 
				<< " err:" << newOvlp.seqDivergence
				<< " checkErr:" << getAlignmentErrEdlib(newOvlp, curSeq, extSeq, 1, false)
				<< " checkErrHpc:" << getAlignmentErrEdlib(newOvlp, curSeq, extSeq, 1, true);
		}
	}*/

	return trimmedAlignments;
}
