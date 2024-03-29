#include "Iterator.h"
#include "SortState.h"
#include <fstream>
#include <vector>

class SortPlan : public Plan
{
	friend class SortIterator;

public:
	SortPlan(Plan *const input, SortState state, std::vector<std::ifstream *> inputFiles, int fileCount, int HDD_10GB_count, bool ifGraceful, int MBOrGBLeft);
	~SortPlan();
	Iterator *init() const;

private:
	Plan *const _input;
	SortState _state;
	std::vector<std::ifstream *> _inputFiles;
	int _fileCount;
	int _HDD_10GB_count;
	bool _ifGraceful;
	int _MBOrGBLeft;

}; // class SortPlan

class SortIterator : public Iterator
{
public:
	SortIterator(SortPlan const *const plan);
	~SortIterator();
	bool next();
	DataRecord *getCurrentRecord();

private:
	SortPlan const *const _plan;
	Iterator *const _input;
	RowCount _consumed, _produced;
	DataRecord *_currentRecord;
	std::vector<std::ifstream *> _inputFiles;
	int _fileCount;
	int _HDD_10GB_count;
	bool _ifGraceful;
	int _MBOrGBLeft;
}; // class SortIterator
