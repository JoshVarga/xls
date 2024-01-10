// Copyright 2023 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// See the License for the specific language governing permissions and
// limitations under the License.

////////////////////////////////////////////////////////////////////////////////
// Bitonic sort
// A parametric function for sort an array of u32.
// The array size must be a power of two.
// The parametric takes the size of the array as parameter, but that size can
// be inferred too.
//
// For example, assuming "array" is of type u32[8].
// Then, bitonic_sort(array) will return the sorted array.
//
// Source: https://en.wikipedia.org/wiki/Bitonic_sorter
////////////////////////////////////////////////////////////////////////////////

import std;

fn swap<B: u32, N: u32>(array: uN[B][N], i: u32, j: u32) -> uN[B][N] {
    let i_element = array[i];
    let j_element = array[j];
    update(update(array, i, j_element), j, i_element)
}

fn bitonic_sort<N: u32, LOG_N: u32 = { std::clog2(N) }>(array: u32[N]) -> u32[N] {
    const_assert!(std::is_pow2<N>());
    let (result, _) = for (_, (array, k)) in u32:0..LOG_N {
        let (result, _) = for (_, (array, j)) in u32:0..LOG_N {
            if (j == u32:0) {
                (array, j)
            } else {
                let result = for (i, array) in u32:0..N {
                    let l = i ^ j;
                    if (l <= i) {
                        (array)
                    } else {
                        if ( (((i & k) == u32:0) && (array[i] > array[l])) || (((i & k) != u32:0) && (array[i] < array[l])) ) {
                            swap(array, i, l)
                        } else {
                            (array)
                        }
                    }
                } (array);
                (result, j / u32:2)
            }
        } ((array, k / u32:2));
        (result, k * u32:2)
    } ((array, u32:2));
    result
}

////////////////////////////////////////////////////////////////////////////////
// Tests
////////////////////////////////////////////////////////////////////////////////

#[test]
fn bitonic_8_test() {
    let _ = assert_eq(
        u32[8]:[
        223,237,289,466,673,686,707,916,
        ],
        bitonic_sort(
        u32[8]:[
        707,223,686,289,237,673,466,916,
        ]));
    ()
}

#[test]
fn bitonic_128_test() {
    let _ = assert_eq(
        u32[128]:[
        4,5,7,10,17,21,24,25,31,35,43,44,45,48,52,52,
        75,79,93,97,102,107,116,123,127,132,138,147,155,160,162,179,
        193,194,195,209,217,233,244,251,278,309,327,333,335,340,362,371,
        392,393,395,399,414,418,419,423,429,433,442,446,447,447,458,458,
        463,467,468,482,502,510,519,522,536,548,560,572,583,593,594,594,
        595,608,608,619,633,642,683,684,685,688,689,701,712,718,728,730,
        732,742,743,772,775,780,782,802,802,809,812,813,835,844,845,848,
        849,857,861,873,880,881,887,891,894,905,915,938,953,956,958,970,
        ],
        bitonic_sort(
        u32[128]:[
        102,132,433,97,802,194,147,251,327,583,730,572,809,24,44,956,
        25,891,467,10,482,45,802,608,813,116,155,848,743,418,107,447,
        21,335,393,689,123,48,887,4,905,633,548,718,812,138,880,560,
        619,127,510,894,593,446,458,233,193,536,953,458,309,160,423,861,
        463,595,52,772,162,938,371,732,5,728,75,684,362,857,7,35,
        780,209,775,447,93,419,915,685,333,392,594,31,17,502,52,608,
        43,244,395,742,712,782,642,414,688,594,970,519,217,195,844,340,
        873,179,429,522,79,468,835,881,399,701,849,278,683,958,845,442,
        ]));
    ()
}

#[test]
fn bitonic_1024_test() {
    let _ = assert_eq(
        u32[1024]:[
        0,2,5,5,6,8,10,10,12,12,14,14,15,16,16,18,
        20,21,23,23,24,25,27,29,30,30,32,33,33,35,35,35,
        35,41,43,44,46,46,47,47,48,49,49,50,51,51,53,53,
        53,56,57,58,59,60,61,62,63,63,63,64,64,65,66,66,
        67,68,69,70,70,72,72,74,74,74,75,75,76,79,80,81,
        81,83,85,86,86,86,87,87,89,90,91,92,92,93,94,97,
        98,98,99,99,99,99,99,101,102,102,104,104,106,111,112,114,
        114,115,115,115,120,121,122,122,124,124,125,125,125,126,129,129,
        129,130,130,131,131,132,132,134,135,136,136,138,139,139,141,143,
        145,145,145,146,148,149,149,149,151,151,152,152,155,155,156,156,
        157,159,159,160,161,162,163,164,164,164,164,164,167,168,170,171,
        171,173,173,174,174,175,175,175,175,176,178,180,180,180,181,182,
        182,184,184,186,186,186,186,186,187,188,192,193,195,196,197,197,
        197,199,199,201,201,202,205,208,210,211,211,214,214,215,215,216,
        218,218,219,221,221,222,225,225,226,227,227,227,230,230,231,231,
        232,233,235,235,237,238,238,238,239,239,242,242,242,242,243,243,
        244,244,246,246,246,247,248,249,251,251,252,252,254,254,255,258,
        258,258,259,259,260,262,262,263,265,265,265,268,268,268,270,270,
        275,278,281,282,283,285,286,288,290,290,291,291,291,291,294,295,
        297,298,301,301,302,302,302,304,304,304,305,306,307,307,307,309,
        312,314,315,315,316,317,317,318,318,319,319,320,321,322,322,324,
        326,326,327,328,328,329,330,330,331,332,335,335,337,339,339,343,
        343,344,345,346,346,349,349,351,351,353,356,357,357,358,360,361,
        362,362,362,364,369,370,372,373,374,374,375,376,377,377,377,377,
        378,378,378,378,379,384,385,387,388,389,390,390,392,393,394,395,
        395,396,397,397,398,398,398,398,399,399,399,400,400,401,403,403,
        404,404,409,411,412,412,412,412,412,413,414,415,417,420,420,420,
        423,424,424,425,426,426,427,429,431,432,433,433,433,433,434,438,
        439,440,440,445,445,445,447,448,448,449,450,452,453,455,455,457,
        457,457,458,459,462,462,462,462,463,466,466,466,466,466,467,468,
        468,469,471,471,471,471,472,473,475,476,476,477,477,478,480,483,
        485,486,488,490,490,491,491,493,493,498,500,503,503,504,504,506,
        507,510,510,511,512,513,514,515,515,515,517,517,518,521,521,521,
        521,522,524,524,524,524,525,528,530,530,531,531,533,533,533,534,
        536,537,537,539,540,541,541,542,543,543,546,546,549,550,551,551,
        552,552,552,553,554,555,556,558,558,559,561,561,562,563,563,563,
        563,564,566,567,567,568,568,569,570,570,570,570,571,572,573,573,
        574,575,575,576,578,581,581,582,583,583,585,585,587,587,587,588,
        589,590,590,591,592,594,594,595,596,596,597,597,599,600,602,604,
        604,605,605,607,608,608,608,610,610,611,612,613,613,613,614,614,
        615,615,615,616,617,618,619,619,624,624,627,627,627,628,628,632,
        632,632,634,634,637,638,639,639,641,641,643,643,643,644,645,647,
        648,648,649,650,650,650,650,652,655,656,656,657,657,658,658,659,
        660,660,664,664,665,666,666,667,667,668,669,670,670,671,671,672,
        672,674,674,675,675,676,676,676,677,677,678,679,679,681,682,683,
        683,684,684,687,687,688,688,689,691,693,693,694,695,697,699,701,
        702,703,703,704,706,707,707,707,707,710,712,713,713,714,715,715,
        715,716,717,717,717,719,721,722,722,723,724,727,728,728,729,729,
        731,732,733,733,737,742,743,743,744,747,749,750,751,751,752,752,
        752,755,755,755,758,759,760,761,762,763,764,765,766,767,768,769,
        769,770,770,771,771,771,773,774,775,775,777,777,778,779,779,783,
        784,785,788,788,788,789,790,790,794,796,796,796,797,798,802,802,
        803,803,806,806,807,808,809,809,812,814,815,815,819,821,823,824,
        824,824,827,828,828,829,829,829,830,830,831,831,831,832,834,835,
        835,836,836,837,840,840,841,842,846,846,848,849,850,850,850,851,
        853,853,853,853,856,856,857,858,860,861,861,863,863,866,867,867,
        867,868,868,870,871,872,874,874,874,877,878,879,879,880,881,883,
        885,885,886,886,886,887,888,888,889,890,891,892,893,893,895,896,
        896,896,896,897,897,897,898,899,901,901,902,902,904,906,908,908,
        908,909,909,910,910,911,912,912,914,915,915,916,919,920,924,924,
        925,926,931,934,934,935,935,935,938,939,940,941,943,945,945,946,
        948,950,951,952,952,952,953,953,957,957,957,959,960,960,962,962,
        963,964,964,965,968,968,969,970,970,971,972,974,977,978,982,984,
        984,984,986,986,987,988,988,988,990,990,992,993,993,996,996,1000,
        ],
        bitonic_sort(
        u32[1024]:[
        812,329,534,315,701,25,345,759,130,433,668,404,993,885,902,412,
        173,23,943,432,796,848,971,87,615,307,762,715,64,448,424,138,
        628,537,80,420,570,605,29,863,559,75,61,828,121,462,802,291,
        566,802,0,924,246,676,139,70,831,669,569,659,704,417,357,83,
        396,286,326,546,362,188,122,390,658,164,301,370,964,638,752,305,
        770,378,50,143,218,149,227,963,639,393,785,703,15,931,752,788,
        401,411,186,171,328,824,521,796,375,384,139,925,344,632,35,567,
        946,531,679,72,556,722,717,130,268,74,860,724,794,46,945,935,
        886,783,699,182,707,732,290,242,101,349,102,504,20,72,572,571,
        106,671,806,915,627,821,331,857,426,737,789,703,552,841,247,506,
        159,581,12,199,697,694,910,400,374,87,934,398,448,594,425,965,
        397,238,385,195,164,395,161,604,974,657,252,63,120,886,769,521,
        568,670,727,478,70,226,399,75,514,112,317,893,829,301,211,624,
        493,729,122,542,214,94,678,866,227,796,749,767,517,758,115,831,
        291,595,676,990,97,815,62,182,920,861,914,674,152,132,242,667,
        908,205,650,710,232,90,403,517,151,664,760,175,291,5,449,399,
        44,242,637,149,996,227,235,468,573,47,924,135,251,953,337,440,
        563,251,148,403,318,155,910,99,254,684,522,515,853,656,335,830,
        92,552,239,321,372,12,608,170,290,378,627,282,48,243,890,339,
        388,33,752,115,466,433,896,239,650,665,968,596,840,69,909,540,
        51,691,466,453,68,581,614,525,788,356,881,594,174,957,64,246,
        159,447,475,252,681,755,512,246,63,808,243,327,346,809,886,298,
        510,712,156,867,414,57,874,733,157,114,583,824,671,536,624,89,
        322,114,268,908,387,902,779,632,152,99,567,543,471,871,940,641,
        533,265,764,610,91,533,524,850,1000,771,939,319,751,316,76,755,
        457,399,906,221,343,145,803,555,434,255,755,235,610,521,201,688,
        238,648,394,790,938,262,265,476,455,613,395,972,815,784,912,648,
        948,412,549,358,679,438,984,524,551,842,467,126,830,328,959,74,
        856,412,314,79,216,982,849,373,304,445,919,863,675,175,332,258,
        457,707,990,660,550,558,462,151,288,652,597,469,242,439,500,275,
        186,302,683,503,376,650,237,136,167,171,952,935,462,377,295,317,
        125,751,343,149,248,647,131,145,24,896,452,35,541,2,18,766,
        988,134,599,891,578,215,775,259,178,607,423,141,790,728,650,173,
        809,351,398,463,528,935,125,49,524,81,357,687,197,66,731,330,
        378,853,129,639,102,953,491,627,81,829,644,518,115,951,10,901,
        307,164,511,632,59,874,656,43,53,897,60,616,596,493,874,504,
        468,412,259,988,186,587,643,934,8,111,404,835,278,868,677,570,
        649,483,872,196,774,675,600,67,98,658,771,619,895,778,400,573,
        136,721,210,850,466,687,552,688,330,916,743,458,827,231,420,214,
        49,294,612,181,99,398,302,777,65,21,592,713,459,831,775,797,
        92,225,618,541,521,351,131,180,590,733,197,530,892,433,666,747,
        30,744,472,164,911,605,315,476,851,984,515,850,576,590,335,14,
        427,125,10,771,562,689,309,546,513,433,33,643,318,244,270,197,
        806,788,591,660,515,853,319,582,702,608,657,471,320,413,420,877,
        218,554,431,346,51,503,615,823,268,798,41,244,23,885,564,471,
        587,124,353,563,312,304,986,281,233,834,533,162,969,490,201,898,
        199,587,180,440,398,429,611,719,585,231,617,861,970,840,202,304,
        397,390,693,604,583,575,531,16,339,836,466,829,867,853,568,856,
        491,260,364,219,561,551,912,5,715,570,563,717,960,634,392,155,
        945,104,887,672,63,285,377,175,486,768,30,349,561,926,132,962,
        27,263,457,184,908,553,962,846,412,628,707,322,970,765,952,666,
        129,466,904,6,66,706,485,888,175,589,379,537,230,763,964,677,
        899,258,896,977,47,283,424,510,619,574,729,750,682,968,836,952,
        473,362,164,35,86,249,208,743,957,192,291,897,524,168,803,445,
        613,415,58,588,722,187,360,828,770,369,85,238,819,585,186,777,
        129,978,676,889,870,716,832,695,728,180,184,992,846,186,163,377,
        667,477,684,306,901,693,455,445,74,714,987,824,641,723,807,507,
        215,477,878,613,462,193,146,374,471,883,543,602,993,297,46,868,
        608,664,377,915,160,563,265,570,761,683,211,893,98,615,814,879,
        222,262,307,361,984,56,93,779,156,258,409,655,713,99,302,707,
        858,225,480,960,270,124,16,950,488,909,378,558,769,53,880,941,
        670,176,498,643,539,614,145,742,221,389,988,99,597,86,672,837,
        324,634,717,254,35,957,897,896,450,104,230,326,53,867,645,174,
        996,715,530,986,773,575,888,86,426,362,674,879,490,32,835,14,
        ]));
    ()
}
