
#include "orderBook.hpp"

/*class solution{
public:
    std::vector<int> twoSum(std::vector<int>const& nums, int target){
        std::unordered_map<int, int> seen;
        for(auto i : nums){
            int p = target - nums[i];

            if(seen.find(p) != seen.end()){
                return {seen[p], i};
            }
            seen[nums[i]];
        }
        return {};
    }
};*/


/*class solution{
    public:
    bool isValid(std::string s){
        std::stack<char> expt;
        for (auto i : s){
            if (i == '(') expt.push(')');
            else if( i == '{')expt.push('}');
            else if(i == '[') expt.push(']');
            else{
                if(expt.empty() || i != expt.top()){
                    return false;
                }
                expt.pop();
            }

        }
        if(!expt.empty()) return false;
        return true;
    }
};*/

/*class Solution {
public:
    std::vector<int> dailyTemperatures(std::vector<int>& temperatures) {
        std::stack<int> stack;
        std::vector<int> answer;
        for(int i{}; i < temperatures.size(); ++i){
        answer.push_back(i);
        int dayCount = 0;   
        while(!stack.empty() && temperatures[i] > stack.top()){
                stack.pop();
                ++dayCount;
        }
        answer[i] = dayCount;
        stack.push(temperatures[i]);

        }
        return answer;
    }
};*/

/*class Solution {
public:
    std::vector<int> dailyTemperatures(std::vector<int>& temperatures) {
        std::stack<int> stack;
        std::vector<int> answer(temperatures.size(),0);
        for(int i{}; i < temperatures.size(); ++i){
        while(!stack.empty() && temperatures[i] > temperatures[stack.top()]){
            //answer.push_back(i - stack.top());
            int day = stack.top();
            stack.pop();
            answer[day] = i - day;
        }
        stack.push(i);

        }
        return answer;
    }
};*/



/*class Solution {
public:
    vector<int> nextGreaterElement(vector<int>& nums1, vector<int>& nums2) {
        std::vector<int> ans (nums1.size(),-1);
        std::stack<int> stack;
        unsigned int i = nums1.size() - 1;

        for (int j = 0; j < nums2.size(); ++j){
            if(!stack.empty() && nums2[j] > nums1[stack.top()]){
                int gElem = stack.top();
                stack.pop();
                ans[gElem] = nums2[j];
            }else{
                ans[i] = -1;
            }

        stack.push(i);
        --i;
        }
         return ans;
    }
};*/

/*class Solution {
public:
    std::vector<vector<string>> groupAnagrams(vector<string>& strs) {
       std::unordered_map<string, vector<string>> map;
        vector<vector<string>> result;

        for (auto s : strs) {
            auto sortedKey = std::sort(s.begin(), s.end);
            map[sortedKey].push_back(s);
        }
        return result;
    }
};*/