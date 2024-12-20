var option_count = 0;
var max_option_count = 32;

var placeholders=[
  "New York",
  "Paris",
  "London",
  "Tokyo",
  "Jakarta",
  "Berlin",
  "Munich",
  "Montreal",
  // TODO: more
];

document.addEventListener('DOMContentLoaded', function(){
  var button = document.getElementById('add-poll-option');
  if (!button) {
    return; 
  }
  button.addEventListener('click', add_candidate);

 add_candidate(); 
 add_candidate(); 
 add_candidate(); 
});

function add_candidate() {
  if (option_count >= max_option_count) {
    return;
  }

  var parent = document.getElementById('poll-form-fieldset');
  var candidate_input = document.createElement('div');

  candidate_input.innerHTML = '<label for="poll">Option: </label>' +
    '<input type="text" name="option" placeholder="' +
(placeholders[option_count] || '') +
    '"/></div>';
  parent.insertBefore(candidate_input, parent.childNodes[2 + option_count]);

  option_count += 1;
}

function raise_option(elem){
  console.log("raise", elem);

  var parent = document.getElementById('poll-options-list');
  for (var i=0;i<parent.childElementCount; i++){
    console.log(i, parent.children[i], parent.children[i] == elem.parentElement);
  }
}

function lower_option(elem){
  console.log("lower", elem);
}
