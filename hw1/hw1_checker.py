import json
import sys

def load_json(file_path):
    with open(file_path, 'r') as file:
        return json.load(file)

def print_wrong(dependence, wrong_set):
    correct_keys = {"src_idx", "dst_idx", "array", "src_stmt", "dst_stmt"}
    dependence_arrow = {"FlowDependence" : "----->", "AntiDependence" : "--A-->", "OutputDependence" : "--O-->"}
    for wrong_frozen_set in wrong_set:
        wrong_dict = dict(wrong_frozen_set)
        keys = wrong_dict.keys()
        if keys != correct_keys:
            print("\t\tEntry: ", wrong_dict)
            if correct_keys - keys:
                print("\t\t\tMissing keys: ", correct_keys - keys)
            if keys - correct_keys:
                print("\t\t\tExtra keys: ", keys - correct_keys)
            print("")
            return
        print("\t\ti={},i={}".format(wrong_dict["src_idx"], wrong_dict["dst_idx"]))
        print("\t\t{}:S{} {} S{}\n".format(wrong_dict["array"], wrong_dict["src_stmt"], dependence_arrow[dependence], wrong_dict["dst_stmt"]))

def count_wrong(dependence, answer_list, output_list):
    answer_set = {frozenset(sorted(d.items())) for d in answer_list}
    output_set = {frozenset(sorted(d.items())) for d in output_list}
    missing_set = answer_set - output_set
    extra_set = output_set - answer_set
    if missing_set:
        print("\tMissing:")
        print_wrong(dependence, missing_set)
    if extra_set:
        print("\tExtra:")
        print_wrong(dependence, extra_set)
    if not missing_set and not extra_set:
        print("\tCorrect!\n")
    return len(missing_set) + len(extra_set)

def num_wrong_dep(answer_json, output_json):
    answer_flow_list = answer_json["FlowDependence"]
    answer_anti_list = answer_json["AntiDependence"]
    answer_output_list = answer_json["OutputDependence"]

    output_flow_list = output_json["FlowDependence"]
    output_anti_list = output_json["AntiDependence"]
    output_output_list = output_json["OutputDependence"]

    print("Flow dependence:")
    num = count_wrong("FlowDependence", answer_flow_list, output_flow_list)
    print("Anti dependence:")
    num += count_wrong("AntiDependence", answer_anti_list, output_anti_list)
    print("Output dependence:")
    num += count_wrong("OutputDependence", answer_output_list, output_output_list)
    print("Score: ", max(100 - num * 20, 0))

def main(answer_file, output_file):
    answer_json = load_json(answer_file)
    output_json = load_json(output_file)

    num_wrong_dep(answer_json, output_json)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 script.py answer.json output.json")
        sys.exit(1)

    answer_file = sys.argv[1]
    output_file = sys.argv[2]
    main(answer_file, output_file)
