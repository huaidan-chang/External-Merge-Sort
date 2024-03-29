#include "Sort.h"
#include "Dram.h"
#include "Leaf.h"
#include "PQ.h"
#include "Scan.h"
#include "TraceFile.h"
#include "global.h"
#include <fstream>
#include <memory>
#include <sstream>

// #define REC_SIZE 1000
#define MB 1000000LL
#define KB 1000

SortPlan::SortPlan(Plan *const input, SortState state, std::vector<std::ifstream *> inputFiles, int fileCount, int HDD_10GB_count, bool ifGraceful, int MBOrGBLeft) : _input(input), _state(state), _inputFiles(inputFiles), _fileCount(fileCount), _HDD_10GB_count(HDD_10GB_count), _ifGraceful(ifGraceful), _MBOrGBLeft(MBOrGBLeft)
{
	// TRACE(true);
} // SortPlan::SortPlan

SortPlan::~SortPlan()
{
	// TRACE(true);
	delete _input;
} // SortPlan::~SortPlan

Iterator *SortPlan::init() const
{
	// TRACE(true);
	return new SortIterator(this);
} // SortPlan::init

void swap(DataRecord &a, DataRecord &b)
{
	a.swapContents(b);
}

int part(DataRecord records[], int lower, int upper)
{
	int left = lower;
	for (int i = lower; i < upper; i++)
	{
		if (std::strcmp(records[i].getIncl(), records[upper].getIncl()) < 0)
		{
			swap(records[i], records[left++]);
		}
	}
	swap(records[left], records[upper]);
	return left;
}

void qs(DataRecord records[], int lower, int upper)
{
	if (lower < upper)
	{
		int pivot = part(records, lower, upper);
		qs(records, lower, pivot - 1);
		qs(records, pivot + 1, upper);
	}
}

bool isPowerOfTwo(int x)
{
	return (x > 0) && ((x & (x - 1)) == 0);
}

SortIterator::SortIterator(SortPlan const *const plan) : _plan(plan), _input(plan->_input->init()), // _input is a ScanPlan, so init() will return ScanIterator
														 _consumed(0), _produced(0), _inputFiles(plan->_inputFiles), _fileCount(plan->_fileCount),
														 _HDD_10GB_count(plan->_HDD_10GB_count), _ifGraceful(plan->_ifGraceful), _MBOrGBLeft(plan->_MBOrGBLeft)
{
	// TRACE(true);
	while (_input->next())
	{
		++_consumed;
	}

	//  run generation phase1
	if (_plan->_state == RUN_PHASE_1)
	{
		DataRecord *records = new DataRecord[_consumed](); // a pointer to 1MB records
		int i = 0;
		int j = _consumed;
		while (j--)
		{
			char *row = new char[record_size];
			_inputFiles[0]->read(row, record_size);
			row[record_size - 2] = '\0'; // last 2 bytes are newline characters
			// Extracting data from the row
			char *incl = new char[incl_size + 1];
			char *mem = new char[mem_size + 1];
			char *mgmt = new char[mgmt_size + 1];

			std::strncpy(incl, row, incl_size);
			incl[incl_size] = '\0';

			std::strncpy(mem, row + incl_size + 1, mem_size);
			mem[mem_size] = '\0';

			std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
			mgmt[mgmt_size] = '\0';

			// Creating a DataRecord
			std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
			records[i++] = *record;
			delete[] incl;
			delete[] mem;
			delete[] mgmt;
			delete[] row;
		}

		qs(records, 0, _consumed - 1);
		// add sorted 1MB to DRAM
		dataRecords.push_back(records);
	}

	// Merge sort 100MB runs in DRAM and output to SSD
	else if (_plan->_state == RUN_PHASE_2)
	{

		// For 1 KB: 1 MB/1 KB = 1,000 records per bucket
		// For 50 bytes: 1 MB/50 bytes = 20,000 records per bucket
		int sizeOfBucket = MB / record_size;
		// Graceful head 100 runs's record number (first 75% stays in DRAM, and output last 25%)
		// 100 - 25 = 75 -> 75 / 100 = 0.75
		// 0.75 * 20,000 records = 15,000 records
		int recordNumHead100 = ((100 - _MBOrGBLeft) / 100) * sizeOfBucket;
		int numRecord_leftOverOf1MB = _consumed % sizeOfBucket;
		// if is graceful, buckets will be 125
		int const buckets = dataRecords.size();
		int copyNum = buckets; // copyNum = buckets
		int targetlevel = 0;

		// table to store the number of records for each buckets
		// useful when there are leftovers (32 * 1000 records, and 1 * 500 records)
		int *bucketSizeTable = new int[buckets]();
		for (int i = 0; i < buckets - 1; i++)
		{
			bucketSizeTable[i] = sizeOfBucket;
		}

		bucketSizeTable[buckets - 1] = numRecord_leftOverOf1MB ? numRecord_leftOverOf1MB : sizeOfBucket;

		while (copyNum >>= 1)
			++targetlevel;

		if (!isPowerOfTwo(buckets))
			targetlevel++;

		// initializes to 0; stores pointer to the next record to be pushed for the leaf
		int *hashtable = new int[buckets]();
		// for how many records already output from each bucket
		int *cntPerBucket = new int[buckets]();

		// graceful: record the position each run in head 100 runs that will be filled from SSD
		int *filltable = new int[100]();

		for (int i = 0; i < 100; ++i)
		{
			filltable[i] = recordNumHead100;
		}

		// buckets from Dram
		for (int i = 0; i < buckets; ++i)
		{
			DataRecord *inner = dataRecords.at(i);
			DataRecord record = inner[0];
			std::string inclString(record.getIncl());
			::leaf[i].assign(std::begin(inclString), std::end(inclString)); // assign only key to leaf
		}

		// capacity 2^targetlevel
		PQ priorityQueue(targetlevel);
		// calculate the ovc
		for (int i = 0; i < buckets; ++i)
		{
			int intValue = ::leaf[i][0] - '0';
			// 907 vs early-fence: arity = 3 (key has 3 columns); offset = 0 (compare with early-fence so differentiating index must be 0)
			// intValue = 9; arity - offset = 3 - 0 = 3; 3 * 100 + 9 = 309
			priorityQueue.push(i, (sizeOfColumn - 0) * 100 + intValue);
		}

		// input is 100,000 records; if each leaf (bucket) contains 1000 records, we only need 100 buckets;
		// for the leftover buckets, fill in late fence and push
		if (buckets < priorityQueue.capacity())
		{
			for (int i = buckets; i < priorityQueue.capacity(); i++)
				priorityQueue.push(i, priorityQueue.late_fence());
		}

		// already push 1 record from each bucket, thus init this array with 1
		for (int i = 0; i < buckets; ++i)
		{
			cntPerBucket[i] = 1;
		}

		std::stringstream filename;
		std::stringstream filename_graceful_hdd;
		// 125MB : left over of 25MB should go to SSD
		// 50MB : should go to output directly
		if (totalSize < 100000000 || (_ifGraceful && totalSize == 125 * MB))
		{
			filename << "output/final_output.txt";
		}
		else if (_ifGraceful)
		{
			// 12GB Graceful degradation
			// output only 2GB to HDD instead the whole 12GB
			filename << "SSD-10GB/output_graceful_" << _fileCount << ".txt";
			filename_graceful_hdd << "HDD/output_graceful_" << _fileCount << ".txt";
		}
		else
		{
			filename << "SSD-10GB/output_" << _fileCount << ".txt";
		}

		std::ofstream outputFile(filename.str(), std::ios::binary | std::ios::app); // std::ios::app for appending
		traceprintf("%s\n", filename.str().c_str());
		if (!outputFile.is_open())
			std::cerr << "Error opening output file." << std::endl;

		// for 12GB graceful degradation output
		std::ofstream outputFile2;
		if (_ifGraceful && buckets == 100)
		{
			outputFile2.open(filename_graceful_hdd.str(), std::ios::binary | std::ios::app);
			(filename_graceful_hdd.str(), std::ios::binary | std::ios::app); // std::ios::app for appending
			traceprintf("%s\n", filename_graceful_hdd.str().c_str());
			if (!outputFile2.is_open())
				std::cerr << "Error opening output file." << std::endl;
		}

		int count = 0; // current count of the records being popped
		while (static_cast<RowCount>(count) < _consumed)
		{
			int idx = priorityQueue.pop();
			if (idx == -1)
				break;

			if (_ifGraceful && buckets == 100 && count >= (80 * MB) / record_size)
			{
				// 12GB Graceful 
				// output last 20MB of each 100MB run to HDD
				DataRecord *inner = dataRecords.at(idx);
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record = inner[hashtable[idx]];
				outputFile2.write(output_record.getIncl(), incl_size);
				outputFile2.write(" ", 1);
				outputFile2.write(output_record.getMem(), mem_size);
				outputFile2.write(" ", 1);
				outputFile2.write(output_record.getMgmt(), mgmt_size);
				outputFile2.write("\r\n", 2);
			}
			else
			{
				// write to the output file
				// inner is a pointer to 1MB records array
				// if total size is 100MB and is graceful means you only need to output
				DataRecord *inner = dataRecords.at(idx);
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record = inner[hashtable[idx]];
				outputFile.write(output_record.getIncl(), incl_size);
				outputFile.write(" ", 1);
				outputFile.write(output_record.getMem(), mem_size);
				outputFile.write(" ", 1);
				outputFile.write(output_record.getMgmt(), mgmt_size);
				outputFile.write("\r\n", 2);
			}

			// 125MB graceful degradation with 125 buckets
			// first 100 runs need to fill 0.25MB for each run from SSD
			if (_ifGraceful && idx < 100 && buckets > 100)
			{
				// fill the 0.25MB of head 100 runs
				// For 50 byte record: 8KB = 8*20 Records
				int numOfRec8KB = 8 * KB / record_size;
				if (cntPerBucket[idx] % numOfRec8KB == 0 && cntPerBucket[idx] <= bucketSizeTable[idx] - recordNumHead100)
				{
					// read 8KB records from SSD
					DataRecord *inner = dataRecords.at(idx);
					int startToFillPointer = filltable[idx];
					for (int j = 0; j < numOfRec8KB; j++)
					{
						char *row = new char[record_size];
						_inputFiles[idx + 1]->read(row, record_size);
						row[record_size - 2] = '\0'; // last 2 bytes are newline characters
						// Extracting data from the row
						char *incl = new char[incl_size + 1];
						char *mem = new char[mem_size + 1];
						char *mgmt = new char[mgmt_size + 1];

						std::strncpy(incl, row, incl_size);
						incl[incl_size] = '\0';

						std::strncpy(mem, row + incl_size + 1, mem_size);
						mem[mem_size] = '\0';

						std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
						mgmt[mgmt_size] = '\0';

						// Creating a DataRecord
						std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
						inner[startToFillPointer++] = *record;
						filltable[idx]++;
						delete[] incl;
						delete[] mem;
						delete[] mgmt;
						delete[] row;
					}
				}
			}

			if (static_cast<RowCount>(count) == _consumed - 1)
				break;
			if (hashtable[idx] < bucketSizeTable[idx] - 1) // check if there are record left in the bucket
			{
				char *preVal = new char[sizeOfColumn + 1];
				// assign the data outputted, so preVal can be used to compare with next value in the same bucket
				std::strcpy(preVal, ::leaf[idx].data());
				hashtable[idx]++;	 // update the pointer
				cntPerBucket[idx]++; // update the count per bucket

				// up till now, the record of the leaf is still the same as the outputted one
				DataRecord *inner_cur = dataRecords.at(idx);   // inner_cur is a pointer to 1000 records, each 1kb
				DataRecord record = inner_cur[hashtable[idx]]; // hashtable[idx] has been updated

				std::strcpy(::leaf[idx].data(), record.getIncl()); // assign new key to leaf
				char *curVal = new char[sizeOfColumn + 1];

				std::strcpy(curVal, ::leaf[idx].data());

				// find the offset
				int offset = -1; // the index of the value that two values start to differ
				for (int i = 0; i < sizeOfColumn; i++)
				{
					if (preVal[i] == curVal[i])
					{
						continue;
					}
					else
					{
						offset = i;
						break;
					}
				}

				// two values of the same bucket are equal
				if (offset == -1)
				{
					offset = sizeOfColumn;
				}

				// if equal, the new value should be pushed right away to the output
				if (offset == sizeOfColumn)
				{
					priorityQueue.push(idx, 0);
				}

				// two values differ, the latter in the bucket will compare with the one popped
				else
				{
					int arityOffset = sizeOfColumn - offset;
					int intValue = curVal[offset] - '0';
					priorityQueue.push(idx, arityOffset * 100 + intValue);
				}

				delete[] preVal;
				delete[] curVal;
			}
			else // no more record left
			{
				priorityQueue.push(idx, priorityQueue.late_fence());
			}
			count++;
		}

		outputFile.close();
		if (outputFile2.is_open())
		{
			outputFile2.close();
		}
		delete[] hashtable;
	}

	/***************************************/
	/*********External Phase 1**************/
	/***************************************/

	// 120GB: merges 100*100MB (10GB) runs from SSD to HDD (and repeate 12 times)
	// 125MB: merges 100MB and 25MB to output
	else if (_plan->_state == EXTERNAL_PHASE_1)
	{
		// mode 0: 2GB or 10xGB, mode 1: 125MB
		// Rec size 1KB, 1GB = 1000*1000KB = 1,000,000 records
		// Rec size 50 bytes, 1GB = 1000*1000KB = 20,000,000 records
		int numOfRec1GB = 1000 * MB / record_size;
		bool mode = _consumed < static_cast<RowCount>(numOfRec1GB); // 1GB

		int numOfRec100MB = MB * 100 / record_size;
		int numOfRec80MB = MB * 80 / record_size;
		int numOfRec20MB = MB * 20 / record_size;
		int numOf100MBruns = _consumed / numOfRec100MB;

		// if handling 100MB's leftover, for example 25MB in 125MB
		// number of bucket should ++
		if (mode)
			numOf100MBruns++;

		// if handling 10GB's leftover, for example 2GB in 12GB
		// 12GB is 120,000,000; devided it by 10GB size of record
		// this 2GB will become another HDD output
		// this variable will be used later to name file
		// Rec size 1KB, 10GB = 10*1000*1000KB = 10,000,000 records
		// int singleDigitGBleft = _consumed % 10000000 == 0 ? 0 : 1;
		long long numOfRec10GB = 10 * 1000 * MB / record_size;
		int singleDigitGBleft = _consumed % numOfRec10GB == 0 ? 0 : 1;
		int numOfRec1MB = MB / record_size;

		// read the 1MB from each 100MB on SSD
		for (int i = 1; i < numOf100MBruns + 1; i++)
		{
			DataRecord *records = new DataRecord[numOfRec1MB]();
			for (int j = 0; j < numOfRec1MB; j++)
			{
				char *row = new char[record_size];
				_inputFiles[i]->read(row, record_size);
				row[record_size - 2] = '\0'; // last 2 bytes are newline characters
				// Extracting data from the row
				char *incl = new char[incl_size + 1];
				char *mem = new char[mem_size + 1];
				char *mgmt = new char[mgmt_size + 1];

				std::strncpy(incl, row, incl_size);
				incl[incl_size] = '\0';

				std::strncpy(mem, row + incl_size + 1, mem_size);
				mem[mem_size] = '\0';

				std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
				mgmt[mgmt_size] = '\0';

				// Creating a DataRecord
				std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
				records[j] = *record;
				delete[] incl;
				delete[] mem;
				delete[] mgmt;
				delete[] row;
			}
			dataRecords.push_back(records);
		}

		// Dram (dataRecords) is 100MB
		// 1 MB = 8KB * 125
		// 8KB = 8 records
		// 8 * 125 records per bucket
		// For record size 50 byte: 8KB = 8*20 records
		int sizeOfBucket = numOfRec1MB;
		int const numOfbuckets = numOf100MBruns; // 100MB/1MB = 100 = fan-in
		int copyNum = numOfbuckets;				 // copyNum = buckets = 100
		int targetlevel = 0;

		int *bucketTotalSizeTable = new int[numOfbuckets]();
		for (int i = 0; i < numOfbuckets - 1; i++)
		{
			if (_ifGraceful && i < 100)
			{
				// 12GB graceful degradation, first 100 runs only have 80MB on SSD
				bucketTotalSizeTable[i] = numOfRec80MB;
			}
			else
			{
				// normal cases each run is 100MB
				bucketTotalSizeTable[i] = numOfRec100MB;
			}
		}

		// 12GB graceful degradation first 100 runs has 20MB on HDD
		// record if the data on HDD have been filled to DRAM
		int *bucketSizeGracefulTable = new int[100]();
		for (int i = 0; i < 100; i++)
		{
			bucketSizeGracefulTable[i] = numOfRec20MB;
		}

		// mode 0: 2GB or 10xGB, mode 1: 125MB
		// bucketTotalSizeTable[numOfbuckets - 1] = mode ? _consumed % 100000 : 100000;
		if (_ifGraceful)
		{
			bucketTotalSizeTable[numOfbuckets - 1] = numOfRec100MB;
		}
		else
		{
			bucketTotalSizeTable[numOfbuckets - 1] = mode ? _consumed % numOfRec100MB : numOfRec100MB;
		}
		traceprintf("=====%i=====\n", bucketTotalSizeTable[1]);

		while (copyNum >>= 1)
			++targetlevel;

		if (!isPowerOfTwo(numOfbuckets))
		{
			targetlevel++;
		}

		// initialized to 0; stores pointer to the next record to be pushed for the leaf
		int *hashtable = new int[numOfbuckets]();

		// for how many records already output from each bucket
		int *cntPerBucket = new int[numOfbuckets]();
		// int *cnt1MBPerBucket = new int[numOfbuckets]();

		// already push 1 record from each bucket, thus init this array with 1
		for (int i = 0; i < numOfbuckets; ++i)
		{
			cntPerBucket[i] = 1;
		}

		// buckets from Dram
		for (int i = 0; i < numOfbuckets; ++i)
		{
			DataRecord *inner = dataRecords.at(i);
			DataRecord record = inner[0];
			std::string inclString(record.getIncl());
			::leaf[i].assign(std::begin(inclString), std::end(inclString)); // assign only key to leaf
		}

		// capacity 2^targetlevel
		PQ priorityQueue(targetlevel);

		// calculate the ovc
		for (int i = 0; i < numOfbuckets; ++i)
		{
			int intValue = ::leaf[i][0] - '0';
			// 907 vs early-fence: arity = 3 (key has 3 columns); offset = 0 (compare with early-fence so differentiating index must be 0)
			// intValue = 9; arity - offset = 3 - 0 = 3; 3 * 100 + 9 = 309
			priorityQueue.push(i, (sizeOfColumn - 0) * 100 + intValue);

			// std::cout << "intValue[" << i << "]: " << intValue << std::endl;
		}

		// input is 100,000 records; if each leaf (bucket) contains 1000 records, we only need 100 buckets;
		// for the leftover buckets, fill in late fence and push
		if (numOfbuckets < priorityQueue.capacity()) // capacity = 128
		{
			for (int i = numOfbuckets; i < priorityQueue.capacity(); i++)
				priorityQueue.push(i, priorityQueue.late_fence());
		}

		std::stringstream HDD_file;
		// mode 0: 2GB or 10xGB, mode 1: 125MB
		if (mode || _ifGraceful)
			HDD_file << "output/final_output.txt";
		else
			HDD_file << "HDD/output_10GB_" << _HDD_10GB_count + singleDigitGBleft << ".txt";
		// TODO: Can only do 12 GB currently

		std::ofstream outputFile(HDD_file.str(), std::ios::binary | std::ios::app); // std::ios::app for appending
		if (!outputFile.is_open())
			std::cerr << "Error opening output file." << std::endl;

		int count = 0; // current count of the records being popped

		// output buffer : 1MB
		DataRecord *outputBuf = new DataRecord[numOfRec1MB]();
		int outputBufCnt = 0;
		// int outputTotalCnt = 0;
		while (static_cast<RowCount>(count) < _consumed)
		{
			int idx = priorityQueue.pop();
			if (idx == -1)
			{
				break;
			}
			if (outputBufCnt < numOfRec1MB)
			{
				DataRecord *inner = dataRecords.at(idx);
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record(inner[hashtable[idx]]);
				// add to the output buffer
				outputBuf[outputBufCnt++] = output_record;
			}
			else
			{
				// if output buffer has 1MB records, write to HDD
				for (int i = 0; i < numOfRec1MB; i++)
				{
					DataRecord output_record = outputBuf[i];
					outputFile.write(output_record.getIncl(), incl_size);
					outputFile.write(" ", 1);
					outputFile.write(output_record.getMem(), mem_size);
					outputFile.write(" ", 1);
					outputFile.write(output_record.getMgmt(), mgmt_size);
					outputFile.write("\r\n", 2);
				}
				outputBufCnt = 0;
				DataRecord *inner = dataRecords.at(idx);
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record(inner[hashtable[idx]]);
				// add to the output buffer
				outputBuf[outputBufCnt++] = output_record;
			}

			if (static_cast<RowCount>(count) == _consumed - 1)
				break;

			// the total size of a bucket is 100MB = 100*1000 records
			// %8 == 0 means current page had all beeb outputted
			// from SSD input another page
			// For 1KB record: 8KB = 8 records
			// For 50 byte record: 8KB = 8*20 Records
			int numOfRec8KB = 8 * KB / record_size;
			int numOfRec1MB = 1 * MB / record_size;
			if (cntPerBucket[idx] % numOfRec8KB == 0 && cntPerBucket[idx] <= bucketTotalSizeTable[idx] - numOfRec1MB)
			{
				// read 8KB = 8 records from SSD
				DataRecord *inner = dataRecords.at(idx);
				int curHtPointer = hashtable[idx];
				// the position to fill from SSD
				int startToFillPointer = curHtPointer - (numOfRec8KB - 1);
				for (int j = 0; j < numOfRec8KB; j++)
				{
					char *row = new char[record_size];
					_inputFiles[idx + 1]->read(row, record_size);
					row[record_size - 2] = '\0'; // last 2 bytes are newline characters
					// Extracting data from the row
					char *incl = new char[incl_size + 1];
					char *mem = new char[mem_size + 1];
					char *mgmt = new char[mgmt_size + 1];

					std::strncpy(incl, row, incl_size);
					incl[incl_size] = '\0';

					std::strncpy(mem, row + incl_size + 1, mem_size);
					mem[mem_size] = '\0';

					std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
					mgmt[mgmt_size] = '\0';

					// Creating a DataRecord
					std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
					inner[startToFillPointer++] = *record;
					delete[] incl;
					delete[] mem;
					delete[] mgmt;
					delete[] row;
				}

				// if (curHtPointer == 999)
				// {
				// 	cnt1MBPerBucket[idx]++;
				// }
			}
			else if (_ifGraceful && idx < 100 && cntPerBucket[idx] >= bucketTotalSizeTable[idx] && cntPerBucket[idx] % numOfRec1MB == 0 && cntPerBucket[idx] <= (bucketSizeGracefulTable[idx] + bucketTotalSizeTable[idx] - numOfRec1MB))
			{
				// 12GB graceful degradation
				// read 1MB from SSD
				DataRecord *inner = dataRecords.at(idx);
				int curHtPointer = hashtable[idx];
				int startToFillPointer = curHtPointer - (numOfRec1MB - 1);
				for (int j = 0; j < numOfRec1MB; j++)
				{
					char *row = new char[record_size];
					_inputFiles[idx + 121]->read(row, record_size);
					row[record_size - 2] = '\0'; // last 2 bytes are newline characters
					// Extracting data from the row
					char *incl = new char[incl_size + 1];
					char *mem = new char[mem_size + 1];
					char *mgmt = new char[mgmt_size + 1];

					std::strncpy(incl, row, incl_size);
					incl[incl_size] = '\0';

					std::strncpy(mem, row + incl_size + 1, mem_size);
					mem[mem_size] = '\0';

					std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
					mgmt[mgmt_size] = '\0';

					// Creating a DataRecord
					std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
					inner[startToFillPointer++] = *record;
					delete[] incl;
					delete[] mem;
					delete[] mgmt;
					delete[] row;
				}
				// if (curHtPointer == 999)
				// {
				// 	cnt1MBPerBucket[idx]++;
				// }
			}

			// current 1MB bucket in Dram is empty, and there are records left in the 100MB: assign the pointer back to the beginning of the bucket
			if (hashtable[idx] == numOfRec1MB - 1 && cntPerBucket[idx] <= numOfRec100MB)
			{
				hashtable[idx] = -1;
			}

			if (hashtable[idx] < sizeOfBucket - 1) // check if there are record left in the bucket
			{
				char *preVal = new char[sizeOfColumn + 1];
				// assign the data outputted, so preVal can be used to compare with next value in the same bucket
				std::strcpy(preVal, ::leaf[idx].data());
				hashtable[idx]++;	 // update the pointer
				cntPerBucket[idx]++; // update the count per bucket

				// up till now, the record of the leaf is still the same as the outputted one
				DataRecord *inner_cur = dataRecords[idx];	   // inner_cur is a pointer to 1000 records, each 1kb
				DataRecord record = inner_cur[hashtable[idx]]; // hashtable[idx] has been updated
				std::strcpy(::leaf[idx].data(), record.getIncl()); // assign new key to leaf
				char *curVal = new char[sizeOfColumn + 1];
				std::strcpy(curVal, ::leaf[idx].data());

				// find the offset
				int offset = -1; // the index of the value that two values start to differ
				for (int i = 0; i < sizeOfColumn; i++)
				{
					if (preVal[i] == curVal[i])
					{
						continue;
					}
					else
					{
						offset = i;
						break;
					}
				}

				// two values of the same bucket are equal
				if (offset == -1)
				{
					offset = sizeOfColumn;
				}

				// if equal, the new value should be pushed right away to the output
				if (offset == sizeOfColumn)
				{
					priorityQueue.push(idx, 0);
				}

				// two values differ, the latter in the bucket will compare with the one popped
				else
				{
					int arityOffset = sizeOfColumn - offset;
					int intValue = curVal[offset] - '0';
					priorityQueue.push(idx, arityOffset * 100 + intValue);
				}

				delete[] preVal;
				delete[] curVal;
			}
			else // no more record left
			{
				priorityQueue.push(idx, priorityQueue.late_fence());
			}
			count++;
		}

		// output last output buffer
		for (int i = 0; i < numOfRec1MB; i++)
		{
			// outputTotalCnt++;
			DataRecord output_record = outputBuf[i];
			outputFile.write(output_record.getIncl(), incl_size);
			outputFile.write(" ", 1);
			outputFile.write(output_record.getMem(), mem_size);
			outputFile.write(" ", 1);
			outputFile.write(output_record.getMgmt(), mgmt_size);
			outputFile.write("\r\n", 2);
		}
		outputFile.close();

		delete[] hashtable;
		delete[] cntPerBucket;
		delete[] outputBuf;

		// if(_ifGraceful){
		// 	for(int i = 0; i < 120; i++) {
		// 		std::cout << "cnt1MBPerBucket["<< i << "]:" << cnt1MBPerBucket[i] << std::endl;
		// 	}
		// }
	}

	/***************************************/
	/*********External Phase 2**************/
	/***********HDD->DRAM->HDD**************/
	/***************************************/

	else if (_plan->_state == EXTERNAL_PHASE_2)
	{
		// check if there's data between 1-10GB left, for example 12.5GB
		long long numOfRec10GB = 10 * 1000 * MB / record_size;
		int numOfRec1MB = MB / record_size;
		int singleDigitGBleft = _consumed % numOfRec10GB == 0 ? 0 : 1;
		int const numOfbuckets = _HDD_10GB_count + singleDigitGBleft; // 12
		int recordPerBucket = (100 / numOfbuckets) * numOfRec1MB;
		// 120GB: (100 / 12) * 1000 records = 8MB  = 8MB; 8MB * 12 (buckets) = 96MB (4MB wasted for dram's 100MB)
		// 110GB: (100 / 11) * 1000 records = 9MB  = 9MB; 9MB * 11 (buckets) = 99MB (1MB wasted for dram's 100MB)
		// 12.5GB: (100 / 2) * 1000 records = 50MB = 50mb; 50MB * 2 (buckets) = 100MB
		int sizeOfBucket = recordPerBucket; // How many records are there in each of Dram's buckets

		// table to store the number of records for each buckets
		// useful when there are leftovers (for example 32,500: 32 * 1000 records, and 1 * 500 records)
		int *bucketTotalSizeTable = new int[numOfbuckets]();
		for (int i = 0; i < numOfbuckets - 1; i++)
		{
			bucketTotalSizeTable[i] = numOfRec10GB; // for normal 10GB sorted file in HDD, size is 10000000
		}
		// if there are leftover, the size of the last bucket will change
		bucketTotalSizeTable[numOfbuckets - 1] = singleDigitGBleft ? _consumed % numOfRec10GB : numOfRec10GB;

		for (int i = 0; i < numOfbuckets; i++) // input data from HDD to dram
		{
			DataRecord *records = new DataRecord[recordPerBucket](); // 8MB / 1000 = 8000
			for (int k = 0; k < recordPerBucket; k++)
			{
				char *row = new char[record_size];
				_inputFiles[i]->read(row, record_size);
				row[record_size - 2] = '\0'; // last 2 bytes are newline characters
				// Extracting data from the row
				char *incl = new char[incl_size + 1];
				char *mem = new char[mem_size + 1];
				char *mgmt = new char[mgmt_size + 1];

				std::strncpy(incl, row, incl_size);
				incl[incl_size] = '\0';

				std::strncpy(mem, row + incl_size + 1, mem_size);
				mem[mem_size] = '\0';

				std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
				mgmt[mgmt_size] = '\0';

				// Creating a DataRecord
				std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
				records[k] = *record;
				delete[] incl;
				delete[] mem;
				delete[] mgmt;
				delete[] row;
			}
			dataRecords.push_back(records);
		}

		int copyNum = numOfbuckets;
		int targetlevel = 0;
		while (copyNum >>= 1)
			++targetlevel;

		if (!isPowerOfTwo(numOfbuckets))
			targetlevel++;

		// initialized to 0; stores pointer to the next record to be pushed for the leaf
		int *hashtable = new int[numOfbuckets]();

		// for how many records already output from each bucket (max would be 10GB / 1KB = 10,000,000 records)
		int *cntPerBucket = new int[numOfbuckets]();

		// already push 1 record from each bucket, thus init this array with 1
		for (int i = 0; i < numOfbuckets; ++i)
			cntPerBucket[i] = 1;

		leaf.resize(numOfbuckets);

		// initialize the tree leaves from dram
		for (int i = 0; i < numOfbuckets; ++i)
		{
			DataRecord *inner = dataRecords.at(i);
			DataRecord record = inner[0];
			std::string inclString(record.getIncl());
			::leaf[i].assign(std::begin(inclString), std::end(inclString)); // assign only key to leaf
		}

		// capacity = 2^targetlevel
		PQ priorityQueue(targetlevel);

		// calculate the ovc
		for (int i = 0; i < numOfbuckets; ++i)
		{
			int intValue = ::leaf[i][0] - '0';
			// 907 vs early-fence: arity = 3 (key has 3 columns); offset = 0 (compare with early-fence so differentiating index must be 0)
			// intValue = 9; arity - offset = 3 - 0 = 3; 3 * 100 + 9 = 309
			priorityQueue.push(i, (sizeOfColumn - 0) * 100 + intValue);
		}

		// for the leftover buckets, fill in late fence and push
		if (numOfbuckets < priorityQueue.capacity())
		{
			for (int i = numOfbuckets; i < priorityQueue.capacity(); i++)
				priorityQueue.push(i, priorityQueue.late_fence());
		}

		// create the final output file
		std::ofstream outputFile("output/final_output.txt", std::ios::binary);
		if (!outputFile.is_open())
			std::cerr << "Error opening input file." << std::endl;
		int count = 0; // current count of the records being popped

		// output buffer : 1MB
		DataRecord *outputBuf = new DataRecord[numOfRec1MB]();
		int outputBufCnt = 0;

		while (static_cast<RowCount>(count) < _consumed)
		{
			int idx = priorityQueue.pop();
			if (idx == -1)
				break;

			// if (outputBufCnt < 1000)
			if (outputBufCnt < numOfRec1MB)
			{
				DataRecord *inner = dataRecords.at(idx); // inner is a pointer to 1000 records, each 1kb; dataRecords store 100 such pointers
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record(inner[hashtable[idx]]);
				// add to the output buffer
				outputBuf[outputBufCnt++] = output_record;
			}
			else
			{
				// if output buffer has 1000 records (1MB), write to HDD
				for (int i = 0; i < numOfRec1MB; i++)
				{
					DataRecord output_record = outputBuf[i];
					outputFile.write(output_record.getIncl(), incl_size);
					outputFile.write(" ", 1);
					outputFile.write(output_record.getMem(), mem_size);
					outputFile.write(" ", 1);
					outputFile.write(output_record.getMgmt(), mgmt_size);
					outputFile.write("\r\n", 2);
				}
				outputBufCnt = 0;

				DataRecord *inner = dataRecords.at(idx);
				// idx tells which leaf; hashtable[idx] returns the next pointer to the record
				DataRecord output_record(inner[hashtable[idx]]);
				// add to the output buffer
				outputBuf[outputBufCnt++] = output_record;
			}

			if (static_cast<RowCount>(count) == _consumed - 1)
				break;

			// %1000 == 0 means current 8MB bucket has 1MB (1000 records) outputted
			// from HDD input another 1MB
			// 10GB is 10,000,000 records; the first 8,000 records was in the initialization of Dram
			if (cntPerBucket[idx] % numOfRec1MB == 0 && cntPerBucket[idx] <= bucketTotalSizeTable[idx] - recordPerBucket) // 9,992,000
			{
				// read 1MB = 1000 records from HDD
				DataRecord *inner = dataRecords.at(idx);
				int curHtPointer = hashtable[idx];
				int startToFillPointer = curHtPointer - (numOfRec1MB - 1); // the records before startToFillPointer have been outputted
				for (int j = 0; j < numOfRec1MB; j++)
				{
					char *row = new char[record_size];
					_inputFiles[idx]->read(row, record_size);
					row[record_size - 2] = '\0'; // last 2 bytes are newline characters
					// Extracting data from the row
					char *incl = new char[incl_size + 1];
					char *mem = new char[mem_size + 1];
					char *mgmt = new char[mgmt_size + 1];

					std::strncpy(incl, row, incl_size);
					incl[incl_size] = '\0';

					std::strncpy(mem, row + incl_size + 1, mem_size);
					mem[mem_size] = '\0';

					std::strncpy(mgmt, row + incl_size + 1 + mem_size + 1, mgmt_size);
					mgmt[mgmt_size] = '\0';

					// Creating a DataRecord
					// DataRecord* record = new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt);
					std::unique_ptr<DataRecord> record(new DataRecord(incl_size + 1, mem_size + 1, mgmt_size + 1, incl, mem, mgmt));
					inner[startToFillPointer++] = *record;
					delete[] incl;
					delete[] mem;
					delete[] mgmt;
					delete[] row;
				}
			}
			// current 8MB bucket in Dram is empty, and there are records left in the 10GB bucket: assign the pointer back to the beginning of the bucket
			if (hashtable[idx] == recordPerBucket - 1 && cntPerBucket[idx] <= numOfRec10GB)
				hashtable[idx] = -1;

			if (hashtable[idx] < sizeOfBucket - 1) // check if there are record left in the bucket
			{
				char *preVal = new char[sizeOfColumn + 1];

				// assign the data outputted, so preVal can be used to compare with next value in the same bucket
				std::strcpy(preVal, ::leaf[idx].data());
				hashtable[idx]++;	 // update the pointer
				cntPerBucket[idx]++; // update the count per bucket

				// up till now, the record of the leaf is still the same as the outputted one
				DataRecord *inner_cur = dataRecords.at(idx);	   // inner_cur is a pointer to 1000 records, each 1kb
				DataRecord record = inner_cur[hashtable[idx]];	   // hashtable[idx] has been updated
				std::strcpy(::leaf[idx].data(), record.getIncl()); // assign new key to leaf
				char *curVal = new char[sizeOfColumn + 1];

				std::strcpy(curVal, ::leaf[idx].data());

				// find the offset
				int offset = -1; // the index of the value that two values start to differ
				for (int i = 0; i < sizeOfColumn; i++)
				{
					if (preVal[i] == curVal[i])
					{
						continue;
					}
					else
					{
						offset = i;
						break;
					}
				}

				// two values of the same bucket are equal
				if (offset == -1)
				{
					offset = sizeOfColumn;
				}

				// if equal, the new value should be pushed right away to the output
				if (offset == sizeOfColumn)
				{
					priorityQueue.push(idx, 0);
				}

				// two values differ, the latter in the bucket will compare with the one popped
				else
				{
					int arityOffset = sizeOfColumn - offset;
					int intValue = curVal[offset] - '0';
					priorityQueue.push(idx, arityOffset * 100 + intValue);
				}

				delete[] preVal;
				delete[] curVal;
			}
			else // no more record left
			{
				priorityQueue.push(idx, priorityQueue.late_fence());
			}

			count++;
		}

		// output last output buffer
		// for (int i = 0; i < 1000; i++)
		for (int i = 0; i < numOfRec1MB; i++)
		{
			// outputTotalCnt++;
			DataRecord output_record = outputBuf[i];
			outputFile.write(output_record.getIncl(), incl_size);
			outputFile.write(" ", 1);
			outputFile.write(output_record.getMem(), mem_size);
			outputFile.write(" ", 1);
			outputFile.write(output_record.getMgmt(), mgmt_size);
			outputFile.write("\r\n", 2);
		}
		outputFile.close();
		delete[] hashtable;
		delete[] cntPerBucket;
		delete[] outputBuf;
	}

	delete _input;

	traceprintf("consumed %lu rows\n",
				(unsigned long)(_consumed));
} // SortIterator::SortIterator

SortIterator::~SortIterator()
{
	TRACE(true);

	traceprintf("produced %lu of %lu rows\n",
				(unsigned long)(_produced),
				(unsigned long)(_consumed));
} // SortIterator::~SortIterator

bool SortIterator::next()
{
	// TRACE(true);

	if (_produced >= _consumed)
		return false;

	++_produced;
	return true;
} // SortIterator::next

DataRecord *SortIterator::getCurrentRecord()
{
	return _currentRecord;
}